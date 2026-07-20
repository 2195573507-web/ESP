# ESP Time Sync

This module adds server-side time sync and latency helpers for ESP32-C5 uploads.

## ESP call flow

1. On boot, call `GET /api/time/now` to read the server Unix timestamp in milliseconds.
2. Optionally call `POST /api/time/ping` to measure the current one-way delay estimate.
3. When uploading BME data to `POST /sensor`, include `device_id`, `esp_time_ms`, and `esp_uptime_ms` when available.

Old sensor uploads that only send temperature, humidity, pressure, and gas data still work.

## APIs

### `GET /api/time/now`

Returns:

```json
{
  "ok": true,
  "server_time_ms": 1710000000000,
  "server_time_iso": "2024-03-09T16:00:00.000Z"
}
```

### `POST /api/time/ping`

Request:

```json
{
  "device_id": "esp32c5-main",
  "esp_send_ms": 1710000000000,
  "esp_uptime_ms": 123456
}
```

Response:

```json
{
  "ok": true,
  "device_id": "esp32c5-main",
  "esp_send_ms": 1710000000000,
  "esp_uptime_ms": 123456,
  "server_recv_ms": 1710000000123,
  "server_reply_ms": 1710000000124,
  "server_time_iso": "2024-03-09T16:00:00.124Z",
  "estimated_one_way_delay_ms": 123
}
```

`device_id` is trimmed and capped at 128 characters. If `esp_send_ms` is missing or not numeric, `estimated_one_way_delay_ms` is `null`.

### `GET /api/time/status`

Dashboard helper endpoint. It returns current server time plus the latest saved `/api/time/ping` result. It does not print the ESP sync log.

## Sensor upload fields

`POST /sensor` accepts these extra optional fields:

```json
{
  "temperature": 26.5,
  "humidity": 58.2,
  "pressure": 1008.5,
  "gas_resistance": 12500,
  "device_id": "esp32c5-main",
  "esp_time_ms": 1710000000000,
  "esp_uptime_ms": 123456
}
```

The server stores `device_id`, `esp_time_ms`, `esp_uptime_ms`, `server_recv_ms`, `server_time_iso`, and `upload_delay_ms`. If `esp_time_ms` is missing or invalid, `upload_delay_ms` is `null`.

## Local testing

Start the server:

```bash
npm start
```

Read server time:

```bash
curl http://localhost:3000/api/time/now
```

Ping latency:

```bash
curl -X POST http://localhost:3000/api/time/ping -H "Content-Type: application/json" -d '{"device_id":"esp32c5-main","esp_send_ms":1710000000000,"esp_uptime_ms":123456}'
```

Upload sensor data:

```bash
curl -X POST http://localhost:3000/sensor -H "Content-Type: application/json" -d '{"temperature":26.5,"humidity":58.2,"pressure":1008.5,"gas_resistance":12500,"device_id":"esp32c5-main","esp_time_ms":1710000000000,"esp_uptime_ms":123456}'
```

Read latest sensor row:

```bash
curl http://localhost:3000/sensor/latest
```
