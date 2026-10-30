**Important implementation notes**

This code deliberately uses a fixed maximum of eight subscriptions and a 2KB incoming payload limit. Those can be increased depending on available RAM.

There are also important lifecycle rules:
- Call init(), start(), stop(), and deinit() from a single application management task. Do not call stop() or deinit() inside MQTT callbacks.
- Treat the topic and payload pointers delivered to on_message as valid only during that callback. Copy them if another task needs the data later.
- Use persistent strings for the broker configuration, CA certificate, and availability settings. Do not supply temporary stack strings.
- mqtt_service_publish() is intended for normal application tasks, not interrupt handlers.
- The publish acknowledgement callback is best-effort. If the application queue is full, acknowledgement notifications may be dropped.
- Reassembly allocates memory dynamically and discards messages exceeding the configured maximum.

For a controlled shutdown, publish a retained "offline" status and allow its QoS 1 acknowledgement before stopping MQTT. The LWT itself is intended for unexpected disconnections. Also, ESP_ERROR_CHECK() in deinit() intentionally treats an unsuccessful MQTT stop as a fatal lifecycle error; you can replace this with application-specific recovery if needed.

https://chatgpt.com/share/6ae22024-33cc-83ec-a378-304f6206d05e
