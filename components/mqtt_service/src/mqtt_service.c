
#include "mqtt_service.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define MQTT_OUTBOX_LIMIT 8192
#define MQTT_WORKER_STACK 4096

static const char *TAG = "MQTT_SERVICE";

typedef enum
{
    WORK_MESSAGE,
    WORK_PUBLISHED,
    WORK_STOP
} work_kind_t;

typedef struct
{
    work_kind_t kind;
    char *topic;
    uint8_t *payload;
    size_t length;
    int qos;
    bool retained;
    int message_id;
} work_item_t;

typedef struct
{
    char topic[MQTT_SERVICE_MAX_TOPIC];
    int qos;
} subscription_t;

typedef struct
{
    esp_mqtt_client_handle_t client;

    QueueHandle_t queue;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t worker_done;

    atomic_bool connected;
    atomic_bool started;
    atomic_bool shutting_down;
    atomic_uint dropped;

    subscription_t subscriptions[MQTT_SERVICE_MAX_SUBS];
    size_t subscription_count;

    // Fragment assembly, owned by MQTT event handler.
    work_item_t incoming;
    size_t received;

    mqtt_message_callback_t on_message;
    mqtt_published_callback_t on_published;

    const char *status_topic;
    const char *online_message;
} mqtt_context_t;

static mqtt_context_t s;

// ---------------------------------------
// Memory helpers
// ---------------------------------------

static void free_item(work_item_t *item)
{
    free(item->topic);
    free(item->payload);
    memset(item, 0, sizeof(*item));
}

static void reset_incoming(void)
{
    free_item(&s.incoming);
    s.received = 0;
}

// ---------------------------------------
// Worker task
// ---------------------------------------

static void mqtt_worker(void *arg)
{
    (void)arg;

    work_item_t item;

    while (xQueueReceive(
               s.queue, &item, portMAX_DELAY) == pdTRUE)
    {

        if (item.kind == WORK_STOP)
        {
            break;
        }

        if (item.kind == WORK_MESSAGE)
        {
            if (s.on_message &&
                !atomic_load(&s.shutting_down))
            {

                s.on_message(
                    item.topic,
                    item.payload,
                    item.length,
                    item.qos,
                    item.retained);
            }

            free_item(&item);
        }

        else if (item.kind == WORK_PUBLISHED)
        {
            if (s.on_published &&
                !atomic_load(&s.shutting_down))
            {

                s.on_published(item.message_id);
            }
        }
    }

    xSemaphoreGive(s.worker_done);
    vTaskDelete(NULL);
}

// ---------------------------------------
// Queue helpers
// ---------------------------------------

static void queue_published(int message_id)
{
    work_item_t item = {
        .kind = WORK_PUBLISHED,
        .message_id = message_id};

    // Never wait for the application worker
    // while executing an MQTT event handler.
    if (xQueueSend(s.queue, &item, 0) != pdTRUE)
    {
        ESP_LOGW(TAG,
                 "Publish acknowledgement queue full");
    }
}

static void queue_incoming(void)
{
    if (xQueueSend(
            s.queue, &s.incoming, 0) != pdTRUE)
    {

        atomic_fetch_add(&s.dropped, 1);

        ESP_LOGW(TAG,
                 "Incoming message dropped: queue full");

        reset_incoming();
        return;
    }

    // Queue now owns the allocated memory.
    memset(&s.incoming, 0, sizeof(s.incoming));
    s.received = 0;
}

// ---------------------------------------
// Fragmented message handling
// ---------------------------------------

static void process_data(
    esp_mqtt_event_handle_t event)
{
    if (event->total_data_len < 0 ||
        event->data_len < 0 ||
        event->current_data_offset < 0)
    {
        reset_incoming();
        return;
    }

    const size_t total =
        (size_t)event->total_data_len;

    const size_t length =
        (size_t)event->data_len;

    const size_t offset =
        (size_t)event->current_data_offset;

    // New MQTT publish message.
    if (offset == 0)
    {
        reset_incoming();

        if (total > MQTT_SERVICE_MAX_PAYLOAD ||
            event->topic_len <= 0 ||
            event->topic_len >= MQTT_SERVICE_MAX_TOPIC ||
            !event->topic)
        {

            ESP_LOGW(TAG,
                     "Incoming message exceeds limits");
            atomic_fetch_add(&s.dropped, 1);
            return;
        }

        s.incoming.topic =
            malloc((size_t)event->topic_len + 1);

        // One extra byte allows optional text use.
        s.incoming.payload = malloc(total + 1);

        if (!s.incoming.topic ||
            !s.incoming.payload)
        {

            ESP_LOGE(TAG, "Allocation failed");
            atomic_fetch_add(&s.dropped, 1);
            reset_incoming();
            return;
        }

        memcpy(
            s.incoming.topic,
            event->topic,
            event->topic_len);

        s.incoming.topic[event->topic_len] = '\0';

        s.incoming.kind = WORK_MESSAGE;
        s.incoming.length = total;
        s.incoming.qos = event->qos;
        s.incoming.retained = event->retain;
        s.received = 0;
    }

    // No active message, malformed fragment,
    // or missing / out-of-order data.
    if (!s.incoming.topic ||
        offset != s.received ||
        offset > s.incoming.length ||
        length > s.incoming.length - offset ||
        (length > 0 && !event->data))
    {

        reset_incoming();
        return;
    }

    if (length > 0)
    {
        memcpy(
            s.incoming.payload + offset,
            event->data,
            length);
    }

    s.received += length;

    if (s.received == s.incoming.length)
    {
        s.incoming.payload[s.received] = '\0';
        queue_incoming();
    }
}

// ---------------------------------------
// MQTT event handler
// ---------------------------------------

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = event_data;

    if (atomic_load(&s.shutting_down))
    {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id)
    {

    case MQTT_EVENT_CONNECTED:
    {
        atomic_store(&s.connected, true);

        ESP_LOGI(TAG, "MQTT connected");

        // Restore subscriptions on every connection.
        xSemaphoreTake(s.mutex, portMAX_DELAY);

        for (size_t i = 0;
             i < s.subscription_count;
             ++i)
        {

            int id = esp_mqtt_client_subscribe(
                s.client,
                s.subscriptions[i].topic,
                s.subscriptions[i].qos);

            if (id < 0)
            {
                ESP_LOGW(TAG,
                         "Subscribe failed: %s",
                         s.subscriptions[i].topic);
            }
        }

        xSemaphoreGive(s.mutex);

        // Update retained device availability.
        if (s.status_topic && s.online_message)
        {
            int id = esp_mqtt_client_enqueue(
                s.client,
                s.status_topic,
                s.online_message,
                (int)strlen(s.online_message),
                1,
                1,
                true);

            if (id < 0)
            {
                ESP_LOGW(TAG,
                         "Unable to queue online status");
            }
        }

        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s.connected, false);
        reset_incoming();

        ESP_LOGW(TAG, "MQTT disconnected");
        break;

    case MQTT_EVENT_DATA:
        process_data(event);
        break;

    case MQTT_EVENT_PUBLISHED:
        queue_published(event->msg_id);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGD(TAG,
                 "Subscription ACK: %d",
                 event->msg_id);
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle)
        {
            ESP_LOGE(TAG,
                     "MQTT error type: %d",
                     event->error_handle->error_type);

            ESP_LOGE(TAG,
                     "TLS verification flags: 0x%x",
                     (unsigned)event->error_handle
                         ->esp_tls_cert_verify_flags);
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------
// Initialize component
// ---------------------------------------

esp_err_t mqtt_service_init(
    const mqtt_service_config_t *config)
{
    if (!config ||
        !config->broker_uri ||
        strncmp(config->broker_uri, "wss://", 6) != 0 ||
        !config->client_id ||
        !config->status_topic ||
        !config->online_message ||
        !config->offline_message)
    {

        return ESP_ERR_INVALID_ARG;
    }

    if (s.client)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s, 0, sizeof(s));

    atomic_init(&s.connected, false);
    atomic_init(&s.started, false);
    atomic_init(&s.shutting_down, false);
    atomic_init(&s.dropped, 0);

    s.on_message = config->on_message;
    s.on_published = config->on_published;
    s.status_topic = config->status_topic;
    s.online_message = config->online_message;

    s.queue = xQueueCreate(
        MQTT_SERVICE_QUEUE_DEPTH,
        sizeof(work_item_t));

    s.mutex = xSemaphoreCreateMutex();
    s.worker_done = xSemaphoreCreateBinary();

    if (!s.queue || !s.mutex || !s.worker_done)
    {
        goto fail;
    }

    esp_mqtt_client_config_t mqtt_config = {
        .broker = {
            .address.uri = config->broker_uri,
            .verification = {
                .certificate = config->ca_cert,
                .crt_bundle_attach =
                    config->ca_cert
                        ? NULL
                        : esp_crt_bundle_attach,
            },
        },

        .credentials = {
            .client_id = config->client_id,
            .username = config->username,
            .authentication.password = config->password,
        },

        .session = {
            .keepalive = 30,
            .last_will = {
                .topic = config->status_topic,
                .msg = config->offline_message,
                .qos = 1,
                .retain = true,
            },
        },

        .network = {
            .reconnect_timeout_ms = 5000,
        },

        .outbox = {
            .limit = MQTT_OUTBOX_LIMIT,
        },
    };

    s.client = esp_mqtt_client_init(&mqtt_config);

    if (!s.client)
    {
        goto fail;
    }

    if (esp_mqtt_client_register_event(
            s.client,
            ESP_EVENT_ANY_ID,
            mqtt_event_handler,
            NULL) != ESP_OK)
    {
        goto fail;
    }

    if (xTaskCreate(
            mqtt_worker,
            "mqtt_worker",
            MQTT_WORKER_STACK,
            NULL,
            5,
            NULL) != pdPASS)
    {
        goto fail;
    }

    ESP_LOGI(TAG, "MQTT service initialized");
    return ESP_OK;

fail:
    if (s.client)
    {
        esp_mqtt_client_destroy(s.client);
    }
    if (s.queue)
    {
        vQueueDelete(s.queue);
    }
    if (s.mutex)
    {
        vSemaphoreDelete(s.mutex);
    }
    if (s.worker_done)
    {
        vSemaphoreDelete(s.worker_done);
    }

    memset(&s, 0, sizeof(s));
    return ESP_ERR_NO_MEM;
}

// ---------------------------------------
// Start / stop
// ---------------------------------------

esp_err_t mqtt_service_start(void)
{
    if (!s.client ||
        atomic_load(&s.shutting_down) ||
        atomic_load(&s.started))
    {

        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_mqtt_client_start(s.client);

    if (err == ESP_OK)
    {
        atomic_store(&s.started, true);
    }

    return err;
}

esp_err_t mqtt_service_stop(void)
{
    if (!s.client)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!atomic_load(&s.started))
    {
        return ESP_OK;
    }

    esp_err_t err = esp_mqtt_client_stop(s.client);

    if (err == ESP_OK)
    {
        atomic_store(&s.started, false);
        atomic_store(&s.connected, false);
        reset_incoming();
    }

    return err;
}

// ---------------------------------------
// Subscription API
// ---------------------------------------

esp_err_t mqtt_service_subscribe(
    const char *topic,
    int qos)
{
    if (!s.client || !topic ||
        !*topic ||
        qos < 0 || qos > 2 ||
        atomic_load(&s.shutting_down))
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(topic);

    if (len >= MQTT_SERVICE_MAX_TOPIC)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s.mutex, portMAX_DELAY);

    // Update an existing subscription if present.
    for (size_t i = 0;
         i < s.subscription_count; ++i)
    {

        if (strcmp(
                s.subscriptions[i].topic, topic) == 0)
        {

            s.subscriptions[i].qos = qos;

            int id = 0;

            if (atomic_load(&s.connected))
            {
                id = esp_mqtt_client_subscribe(
                    s.client, topic, qos);
            }

            xSemaphoreGive(s.mutex);

            return id >= 0 ? ESP_OK : ESP_FAIL;
        }
    }

    if (s.subscription_count >=
        MQTT_SERVICE_MAX_SUBS)
    {

        xSemaphoreGive(s.mutex);
        return ESP_ERR_NO_MEM;
    }

    subscription_t *sub =
        &s.subscriptions[s.subscription_count++];

    memcpy(sub->topic, topic, len + 1);
    sub->qos = qos;

    int id = 0;

    if (atomic_load(&s.connected))
    {
        id = esp_mqtt_client_subscribe(
            s.client, topic, qos);
    }

    xSemaphoreGive(s.mutex);

    return id >= 0 ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------
// Publishing API
// ---------------------------------------

esp_err_t mqtt_service_publish(
    const char *topic,
    const void *payload,
    size_t payload_len,
    int qos,
    bool retain,
    int *out_message_id)
{
    if (out_message_id)
    {
        *out_message_id = -1;
    }

    if (!s.client || !topic || !*topic ||
        (payload_len > 0 && !payload) ||
        payload_len > INT_MAX ||
        qos < 0 || qos > 2 ||
        atomic_load(&s.shutting_down))
    {

        return ESP_ERR_INVALID_ARG;
    }

    if (!atomic_load(&s.started))
    {
        return ESP_ERR_INVALID_STATE;
    }

    int id = esp_mqtt_client_enqueue(
        s.client,
        topic,
        payload ? (const char *)payload : "",
        (int)payload_len,
        qos,
        retain ? 1 : 0,
        true);

    if (id == -2)
    {
        ESP_LOGW(TAG, "MQTT outbox full");
        return ESP_ERR_NO_MEM;
    }

    if (id < 0)
    {
        return ESP_FAIL;
    }

    if (out_message_id)
    {
        *out_message_id = id;
    }

    return ESP_OK;
}

// ---------------------------------------
// Status / statistics
// ---------------------------------------

bool mqtt_service_is_connected(void)
{
    return atomic_load(&s.connected);
}

uint32_t mqtt_service_dropped_messages(void)
{
    return atomic_load(&s.dropped);
}

// ---------------------------------------
// Clean shutdown
// ---------------------------------------

void mqtt_service_deinit(void)
{
    if (!s.client)
    {
        return;
    }

    atomic_store(&s.shutting_down, true);

    // Must be called outside MQTT event handler
    // and outside mqtt_worker callbacks.
    if (atomic_load(&s.started))
    {
        ESP_ERROR_CHECK(mqtt_service_stop());
    }

    esp_mqtt_client_destroy(s.client);
    s.client = NULL;

    // Stop the worker after draining queued items.
    work_item_t stop_item = {
        .kind = WORK_STOP};

    xQueueSend(
        s.queue,
        &stop_item,
        portMAX_DELAY);

    xSemaphoreTake(
        s.worker_done,
        portMAX_DELAY);

    reset_incoming();

    vQueueDelete(s.queue);
    vSemaphoreDelete(s.mutex);
    vSemaphoreDelete(s.worker_done);

    memset(&s, 0, sizeof(s));

    ESP_LOGI(TAG, "MQTT service destroyed");
}
