/**
 * @file command_router.c
 * @brief S3 网关命令路由和轻量命令映射。
 *
 * 本文件属于 ESPS3 网关，负责从 Server 拉取完整命令、映射为 C5 本地命令码、
 * 管理短队列，并把 C5 ack 映射回 Server ack。它不执行命令、不驱动 LCD/扬声器，
 * 也不改变 C5 本地 command 字段定义。
 */

#include "command_router.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "child_registry.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_config.h"
#include "network_worker.h"
#include "offline_policy.h"
#include "protocol_adapter.h"
#include "resource_manager.h"
#include "sensor_aggregator.h"
#include "server_client.h"

static const char *TAG = "command_router";

typedef enum {
    COMMAND_STATE_EMPTY = 0,
    COMMAND_STATE_QUEUED,
    COMMAND_STATE_DISPATCHED,
    COMMAND_STATE_ACKED,
    COMMAND_STATE_FAILED,
    COMMAND_STATE_TIMEOUT,
} command_state_t;

typedef struct {
    char command_id[40];
    char target_device_id[CHILD_REGISTRY_DEVICE_ID_LEN];
    char command_type[40];
    char params_json[512];
    char source[24];
    command_state_t state;
    uint32_t seq;
    uint32_t ttl_ms;
    uint32_t voice_generation;
    uint32_t wake_stream_id;
    int64_t created_ms;
    int64_t dispatched_ms;
} command_entry_t;

static command_entry_t s_queue[GATEWAY_CONFIG_COMMAND_QUEUE_SIZE];
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_poll_lock;
static uint32_t s_command_seq;
static child_registry_entry_t s_poll_entries[GATEWAY_CONFIG_MAX_CHILDREN];
static char s_poll_server_body[SERVER_CLIENT_SMALL_BODY_BYTES];
static bool s_peer_active[GATEWAY_CONFIG_MAX_CHILDREN];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static size_t peer_index(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return GATEWAY_CONFIG_MAX_CHILDREN;
    }

    const gateway_runtime_config_t *config = gateway_config_get();
    size_t count = config->children_allowlist_count;
    if (count > GATEWAY_CONFIG_MAX_CHILDREN) {
        count = GATEWAY_CONFIG_MAX_CHILDREN;
    }
    for (size_t i = 0; i < count; ++i) {
        if (config->children_allowlist[i] != NULL &&
            strcmp(config->children_allowlist[i], device_id) == 0) {
            return i;
        }
    }
    return GATEWAY_CONFIG_MAX_CHILDREN;
}

static bool peer_active_locked(const char *device_id)
{
    const size_t index = peer_index(device_id);
    return index < GATEWAY_CONFIG_MAX_CHILDREN && s_peer_active[index];
}

static bool command_poll_cancelled(void *ctx)
{
    const char *device_id = (const char *)ctx;
    return !command_router_peer_active(device_id) ||
           !resource_manager_is_live(device_id);
}

static command_entry_t *find_locked(const char *command_id)
{
    for (size_t i = 0; i < GATEWAY_CONFIG_COMMAND_QUEUE_SIZE; i++) {
        if (s_queue[i].state != COMMAND_STATE_EMPTY &&
            strcmp(s_queue[i].command_id, command_id) == 0) {
            return &s_queue[i];
        }
    }
    return NULL;
}

static command_entry_t *allocate_locked(void)
{
    for (size_t i = 0; i < GATEWAY_CONFIG_COMMAND_QUEUE_SIZE; i++) {
        if (s_queue[i].state == COMMAND_STATE_EMPTY ||
            s_queue[i].state == COMMAND_STATE_ACKED ||
            s_queue[i].state == COMMAND_STATE_FAILED ||
            s_queue[i].state == COMMAND_STATE_TIMEOUT) {
            return &s_queue[i];
        }
    }
    return NULL;
}

static esp_err_t enqueue_locked(const char *command_id,
                                const char *target_device_id,
                                const char *command_type,
                                const char *params_json,
                                const char *source,
                                uint32_t ttl_ms)
{
    const char *params = params_json != NULL ? params_json : "{}";
    if (strlen(params) >= sizeof(((command_entry_t *)0)->params_json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (command_id != NULL && command_id[0] != '\0' && find_locked(command_id) != NULL) {
        return ESP_OK;
    }

    command_entry_t *entry = allocate_locked();
    if (entry == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(entry, 0, sizeof(*entry));
    entry->seq = ++s_command_seq;
    if (command_id != NULL && command_id[0] != '\0') {
        strlcpy(entry->command_id, command_id, sizeof(entry->command_id));
    } else {
        snprintf(entry->command_id, sizeof(entry->command_id), "local-%u", (unsigned int)entry->seq);
    }
    strlcpy(entry->target_device_id, target_device_id, sizeof(entry->target_device_id));
    strlcpy(entry->command_type, command_type, sizeof(entry->command_type));
    strlcpy(entry->params_json, params, sizeof(entry->params_json));
    strlcpy(entry->source, source != NULL ? source : "local", sizeof(entry->source));
    entry->ttl_ms = ttl_ms > 0 ? ttl_ms : 30000U;
    entry->state = COMMAND_STATE_QUEUED;
    entry->created_ms = now_ms();
    return ESP_OK;
}

static const char *map_server_command_type(const char *server_type)
{
    if (server_type == NULL || server_type[0] == '\0') {
        return "device.noop";
    }
    if (strcmp(server_type, "display.show_text") == 0) {
        return "lcd.show_text";
    }
    if (strcmp(server_type, "alert.play_tone") == 0) {
        return "speaker.play_audio";
    }
    if (strcmp(server_type, "voice.set_volume") == 0) {
        return "speaker.set_volume";
    }
    if (strcmp(server_type, "sensor.set_upload_interval") == 0) {
        return "config.set";
    }
    return server_type;
}

static unsigned int map_local_command_code(const char *command_type)
{
    if (command_type == NULL || command_type[0] == '\0' ||
        strcmp(command_type, "device.noop") == 0) {
        return ESP111_PROTOCOL_LOCAL_COMMAND_NOOP;
    }
    if (strcmp(command_type, "lcd.show_text") == 0 ||
        strcmp(command_type, "display.show_text") == 0) {
        return ESP111_PROTOCOL_LOCAL_COMMAND_SHOW_TEXT;
    }
    if (strcmp(command_type, "speaker.play_audio") == 0) {
        return ESP111_PROTOCOL_LOCAL_COMMAND_PLAY_AUDIO;
    }
    if (strcmp(command_type, "speaker.set_volume") == 0) {
        return ESP111_PROTOCOL_LOCAL_COMMAND_SET_VOLUME;
    }
    if (strcmp(command_type, "config.set") == 0) {
        return ESP111_PROTOCOL_LOCAL_COMMAND_CONFIG_SET;
    }
    if (strcmp(command_type, "voice.start_command_capture") == 0) {
        return ESP111_PROTOCOL_LOCAL_COMMAND_START_COMMAND_CAPTURE;
    }
    return ESP111_PROTOCOL_LOCAL_COMMAND_UNSUPPORTED;
}

static const char *map_local_error_code(unsigned int code)
{
    switch (code) {
    case ESP111_PROTOCOL_LOCAL_ERROR_NONE:
        return "";
    case ESP111_PROTOCOL_LOCAL_ERROR_UNSUPPORTED_COMMAND:
        return ESP111_PROTOCOL_ERROR_UNSUPPORTED_COMMAND;
    case ESP111_PROTOCOL_LOCAL_ERROR_INVALID_PAYLOAD:
        return ESP111_PROTOCOL_ERROR_INVALID_COMMAND_PAYLOAD;
    case ESP111_PROTOCOL_LOCAL_ERROR_TIMEOUT:
        return ESP111_PROTOCOL_ERROR_TIMEOUT;
    case ESP111_PROTOCOL_LOCAL_ERROR_COMMAND_FAILED:
        return ESP111_PROTOCOL_ERROR_COMMAND_FAILED;
    default:
        return ESP111_PROTOCOL_ERROR_UNKNOWN;
    }
}

static esp_err_t command_router_ingest_server_pending(const char *device_id, const char *server_body)
{
    if (device_id == NULL || server_body == NULL || server_body[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!command_router_peer_active(device_id) ||
        !resource_manager_is_live(device_id)) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(server_body);
    if (root == NULL) {
        ESP_LOGW(TAG, "server command response parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
    if (!cJSON_IsArray(commands)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t first_error = ESP_OK;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, commands) {
        cJSON *command_id = cJSON_GetObjectItemCaseSensitive(item, "command_id");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name)) {
            name = cJSON_GetObjectItemCaseSensitive(item, "command_type");
        }
        if (!cJSON_IsString(command_id) || command_id->valuestring == NULL ||
            !cJSON_IsString(name) || name->valuestring == NULL) {
            continue;
        }

        cJSON *payload = cJSON_GetObjectItemCaseSensitive(item, "payload");
        if (payload == NULL) {
            payload = cJSON_GetObjectItemCaseSensitive(item, "params");
        }
        char *params = payload != NULL ? cJSON_PrintUnformatted(payload) : NULL;
        const char *params_json = params != NULL ? params : "{}";

        uint32_t ttl_ms = 30000U;
        cJSON *ttl = cJSON_GetObjectItemCaseSensitive(item, "ttl_ms");
        if (cJSON_IsNumber(ttl) && ttl->valueint > 0) {
            ttl_ms = (uint32_t)ttl->valueint;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        esp_err_t ret = peer_active_locked(device_id) ?
                            enqueue_locked(command_id->valuestring,
                                           device_id,
                                           map_server_command_type(name->valuestring),
                                           params_json,
                                           "server",
                                           ttl_ms) :
                            ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_lock);
        if (ret != ESP_OK && first_error == ESP_OK) {
            first_error = ret;
        } else if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "server command queued id=%s target=%s type=%s",
                     command_id->valuestring,
                     device_id,
                     map_server_command_type(name->valuestring));
        }
        if (params != NULL) {
            cJSON_free(params);
        }
    }

    cJSON_Delete(root);
    return first_error;
}

esp_err_t command_router_init(void)
{
    if (s_lock != NULL && s_poll_lock != NULL) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_poll_lock == NULL) {
        s_poll_lock = xSemaphoreCreateMutex();
        if (s_poll_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    memset(s_queue, 0, sizeof(s_queue));
    memset(s_peer_active, 0, sizeof(s_peer_active));
    s_command_seq = 0;
    ESP_LOGI(TAG, "local command queue initialized size=%u",
             (unsigned int)GATEWAY_CONFIG_COMMAND_QUEUE_SIZE);
    return ESP_OK;
}

esp_err_t command_router_suspend_peer(const char *device_id)
{
    const size_t index = peer_index(device_id);
    if (index >= GATEWAY_CONFIG_MAX_CHILDREN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_peer_active[index] = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t command_router_restore_peer(const char *device_id)
{
    const size_t index = peer_index(device_id);
    if (index >= GATEWAY_CONFIG_MAX_CHILDREN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_peer_active[index] = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool command_router_peer_active(const char *device_id)
{
    if (s_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool active = peer_active_locked(device_id);
    xSemaphoreGive(s_lock);
    return active;
}

bool command_router_has_active_peers(void)
{
    if (s_lock == NULL) {
        return false;
    }

    bool active = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        if (s_peer_active[i]) {
            active = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return active;
}

esp_err_t command_router_enqueue(const char *target_device_id,
                                 const char *command_type,
                                 const char *params_json,
                                 const char *source)
{
    return command_router_enqueue_with_id(target_device_id,
                                          command_type,
                                          params_json,
                                          source,
                                          NULL,
                                          0U);
}

esp_err_t command_router_enqueue_with_id(const char *target_device_id,
                                         const char *command_type,
                                         const char *params_json,
                                         const char *source,
                                         char *out_command_id,
                                         size_t out_command_id_size)
{
    if (!child_registry_is_allowed(target_device_id) || command_type == NULL ||
        command_type[0] == '\0' ||
        (out_command_id != NULL &&
         out_command_id_size < sizeof("local-4294967295"))) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = enqueue_locked(NULL, target_device_id, command_type, params_json, source, 30000U);
    if (ret == ESP_OK && out_command_id != NULL) {
        const int written = snprintf(out_command_id,
                                     out_command_id_size,
                                     "local-%u",
                                     (unsigned int)s_command_seq);
        if (written <= 0 || written >= (int)out_command_id_size) {
            ret = ESP_ERR_INVALID_SIZE;
        }
    }
    xSemaphoreGive(s_lock);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "queued local command target=%s type=%s", target_device_id, command_type);
    return ESP_OK;
}

esp_err_t command_router_enqueue_voice_command_capture(const char *target_device_id,
                                                        const char *command_id,
                                                        uint32_t generation,
                                                        uint32_t wake_stream_id,
                                                        uint32_t timeout_ms)
{
    if (!child_registry_is_allowed(target_device_id) || command_id == NULL ||
        command_id[0] == '\0' || generation == 0U || wake_stream_id == 0U ||
        timeout_ms == 0U || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = enqueue_locked(command_id,
                                   target_device_id,
                                   "voice.start_command_capture",
                                   "{}",
                                   "wake_event",
                                   timeout_ms);
    command_entry_t *entry = ret == ESP_OK ? find_locked(command_id) : NULL;
    if (entry == NULL && ret == ESP_OK) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        entry->voice_generation = generation;
        entry->wake_stream_id = wake_stream_id;
        const int written = snprintf(entry->params_json,
                                     sizeof(entry->params_json),
                                     "{\"command_id\":\"%s\",\"generation\":%lu,"
                                     "\"wake_stream_id\":%lu,\"state\":\"CREATE\","
                                     "\"command_generation\":%lu,\"command_timeout_ms\":%lu}",
                                     entry->command_id,
                                     (unsigned long)generation,
                                     (unsigned long)wake_stream_id,
                                     (unsigned long)generation,
                                     (unsigned long)timeout_ms);
        if (written <= 0 || written >= (int)sizeof(entry->params_json)) {
            memset(entry, 0, sizeof(*entry));
            ret = ESP_ERR_INVALID_SIZE;
        }
    }
    xSemaphoreGive(s_lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "VOICE_COMMAND_SESSION state=CREATE command_id=%s generation=%lu wake_stream_id=%lu target=%s",
                 command_id,
                 (unsigned long)generation,
                 (unsigned long)wake_stream_id,
                 target_device_id);
    }
    return ret;
}

esp_err_t command_router_ack_local_voice(const char *command_id,
                                         const char *device_id,
                                         uint32_t generation,
                                         bool accepted)
{
    if (command_id == NULL || command_id[0] == '\0' || device_id == NULL ||
        device_id[0] == '\0' || generation == 0U || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    command_entry_t *entry = find_locked(command_id);
    if (entry != NULL && strcmp(entry->command_type, "voice.start_command_capture") == 0) {
        if (entry->state != COMMAND_STATE_DISPATCHED ||
            strcmp(entry->target_device_id, device_id) != 0 ||
            entry->voice_generation != generation) {
            ret = ESP_ERR_NOT_ALLOWED;
        } else {
            entry->state = accepted ? COMMAND_STATE_ACKED : COMMAND_STATE_FAILED;
            ret = ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "VOICE_COMMAND_SESSION state=%s reason=command_ack command_id=%s generation=%lu target=%s",
                 accepted ? "CAPTURE_READY" : "FAILED",
                 command_id,
                 (unsigned long)generation,
                 device_id);
    }
    return ret;
}

static void command_router_poll_server_pending_for_device(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0' ||
        !command_router_peer_active(device_id) ||
        !resource_manager_is_live(device_id)) {
        return;
    }

    s_poll_server_body[0] = '\0';
    int server_status = 0;
    esp_err_t server_ret = server_client_get_pending_commands_cancellable(
        device_id,
        s_poll_server_body,
        sizeof(s_poll_server_body),
        &server_status,
        command_poll_cancelled,
        (void *)device_id);
    if (!command_router_peer_active(device_id) ||
        !resource_manager_is_live(device_id)) {
        return;
    }
    offline_policy_record_server_result(server_ret, server_status);
    if (server_ret != ESP_OK) {
        ESP_LOGD(TAG,
                 "server pending command poll failed target=%s status=%d ret=%s",
                 device_id,
                 server_status,
                 esp_err_to_name(server_ret));
        return;
    }

    esp_err_t ingest_ret = command_router_ingest_server_pending(device_id,
                                                                s_poll_server_body);
    if (ingest_ret != ESP_OK && ingest_ret != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG,
                 "server pending command ingest failed target=%s ret=%s",
                 device_id,
                 esp_err_to_name(ingest_ret));
    }
}

void command_router_poll_server_pending(void)
{
    if (s_poll_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_poll_lock, portMAX_DELAY);
    size_t count = child_registry_snapshot(s_poll_entries, GATEWAY_CONFIG_MAX_CHILDREN);
    for (size_t i = 0; i < count; ++i) {
        if (!s_poll_entries[i].registered || !s_poll_entries[i].online ||
            s_poll_entries[i].device_id[0] == '\0' ||
            !command_router_peer_active(s_poll_entries[i].device_id) ||
            !resource_manager_is_live(s_poll_entries[i].device_id)) {
            continue;
        }

        command_router_poll_server_pending_for_device(s_poll_entries[i].device_id);
    }
    xSemaphoreGive(s_poll_lock);
}

esp_err_t command_router_build_pending_json(const char *device_id, char *out, size_t out_size)
{
    if (!child_registry_is_allowed(device_id) || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t local_id = protocol_adapter_device_id_to_local_id(device_id);
    if (local_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = 0;
    int written = snprintf(out,
                           out_size,
                           "{\"" ESP111_PROTOCOL_LOCAL_JSON_OK "\":1,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_ID "\":%u,"
                           "\"" ESP111_PROTOCOL_LOCAL_JSON_COMMANDS "\":[",
                           (unsigned int)local_id);
    if (written <= 0 || written >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    len = (size_t)written;

    bool first = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (peer_active_locked(device_id) && resource_manager_is_live(device_id)) {
        for (size_t i = 0; i < GATEWAY_CONFIG_COMMAND_QUEUE_SIZE; i++) {
            command_entry_t *entry = &s_queue[i];
            if (entry->state != COMMAND_STATE_QUEUED ||
                strcmp(entry->target_device_id, device_id) != 0) {
                continue;
            }

            written = snprintf(out + len,
                               out_size - len,
                               "%s{\"" ESP111_PROTOCOL_LOCAL_JSON_COMMAND_ID "\":\"%s\","
                               "\"" ESP111_PROTOCOL_LOCAL_JSON_COMMAND_CODE "\":%u,"
                               "\"seq\":%u,\"ttl_ms\":%u,"
                               "\"" ESP111_PROTOCOL_LOCAL_JSON_COMMAND_ARGS "\":%s}",
                               first ? "" : ",",
                               entry->command_id,
                               map_local_command_code(entry->command_type),
                               (unsigned int)entry->seq,
                               (unsigned int)entry->ttl_ms,
                               entry->params_json[0] != '\0' ? entry->params_json : "{}");
            if (written <= 0 || written >= (int)(out_size - len)) {
                xSemaphoreGive(s_lock);
                return ESP_ERR_INVALID_SIZE;
            }
            len += (size_t)written;
            first = false;
            entry->state = COMMAND_STATE_DISPATCHED;
            entry->dispatched_ms = now_ms();
            if (strcmp(entry->command_type, "voice.start_command_capture") == 0) {
                ESP_LOGI(TAG,
                         "VOICE_COMMAND_SESSION state=WAIT_ACK command_id=%s generation=%lu wake_stream_id=%lu target=%s",
                         entry->command_id,
                         (unsigned long)entry->voice_generation,
                         (unsigned long)entry->wake_stream_id,
                         entry->target_device_id);
            }
        }
    }
    xSemaphoreGive(s_lock);

    written = snprintf(out + len,
                       out_size - len,
                       "]}");
    return written > 0 && written < (int)(out_size - len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t command_router_ack(const char *command_id, const char *ack_body)
{
    if (command_id == NULL || command_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char ack_device_id[CHILD_REGISTRY_DEVICE_ID_LEN] = {0};
    unsigned int command_code = ESP111_PROTOCOL_LOCAL_COMMAND_UNSUPPORTED;
    bool found = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    command_entry_t *entry = find_locked(command_id);
    if (entry != NULL) {
        entry->state = COMMAND_STATE_ACKED;
        strlcpy(ack_device_id, entry->target_device_id, sizeof(ack_device_id));
        command_code = map_local_command_code(entry->command_type);
        found = true;
    }
    xSemaphoreGive(s_lock);

    if (!found) {
        ESP_LOGW(TAG, "ack for unknown local command id=%s", command_id);
    }

    char server_ack[512];
    const char *ack_status = "failed";
    const char *error_code = ESP111_PROTOCOL_ERROR_ACK_FAILED;
    const char *message = "local command ack failed";
    cJSON *root = ack_body != NULL ? cJSON_Parse(ack_body) : NULL;
    if (root != NULL) {
        if (ack_device_id[0] == '\0') {
            cJSON *local_id = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ID);
            if (cJSON_IsNumber(local_id)) {
                const char *mapped_device_id =
                    protocol_adapter_local_device_id_to_device_id((uint8_t)local_id->valueint);
                if (mapped_device_id != NULL) {
                    strlcpy(ack_device_id, mapped_device_id, sizeof(ack_device_id));
                }
            }
        }
        cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
        cJSON *local_status = cJSON_GetObjectItemCaseSensitive(root, "status");
        if ((cJSON_IsBool(ok) && cJSON_IsTrue(ok)) ||
            (cJSON_IsNumber(ok) && ok->valueint != 0) ||
            (cJSON_IsString(local_status) && local_status->valuestring != NULL &&
             (strcmp(local_status->valuestring, "applied") == 0 ||
              strcmp(local_status->valuestring, "completed") == 0))) {
            ack_status = "completed";
            error_code = "";
            message = "";
        } else {
            cJSON *code = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_LOCAL_JSON_ERROR);
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, ESP111_PROTOCOL_JSON_MESSAGE);
            if (cJSON_IsNumber(code)) {
                error_code = map_local_error_code((unsigned int)code->valueint);
                message = error_code[0] != '\0' ? error_code : "";
            }
            if (cJSON_IsString(code) && code->valuestring != NULL && code->valuestring[0] != '\0') {
                error_code = code->valuestring;
            }
            if (cJSON_IsString(msg) && msg->valuestring != NULL && msg->valuestring[0] != '\0') {
                message = msg->valuestring;
            }
        }
    }

    int written = snprintf(server_ack,
                           sizeof(server_ack),
                           "{\"status\":\"%s\",\"error_code\":\"%s\",\"error_message\":\"%s\","
                           "\"result\":{\"applied\":%s,\"gateway_id\":\"%s\"}}",
                           ack_status,
                           error_code,
                           message,
                           strcmp(ack_status, "completed") == 0 ? "true" : "false",
                           gateway_config_get()->gateway_id);
    if (root != NULL) {
        cJSON_Delete(root);
    }
    if (written <= 0 || written >= (int)sizeof(server_ack)) {
        return ESP_ERR_INVALID_SIZE;
    }

    int http_status = 0;
    esp_err_t ret = network_worker_enqueue_command_ack(command_id, server_ack);
    if (ret != ESP_OK) {
        offline_policy_record_server_result(ret, http_status);
    }
    if (ack_device_id[0] != '\0') {
        sensor_aggregator_record_command_ack(ack_device_id,
                                             command_id,
                                             command_code,
                                             strcmp(ack_status, "completed") == 0);
    }
    ESP_LOGI(TAG, "command ack id=%s local_known=%d queued=%d",
             command_id,
             found ? 1 : 0,
             ret == ESP_OK ? 1 : 0);
    return ESP_OK;
}
