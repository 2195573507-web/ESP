/**
 * @file child_registry.c
 * @brief S3 网关子设备注册表。
 *
 * 本文件属于 ESPS3 网关，负责记录 allowlist 内 C5 的注册信息、最后心跳和在线状态。
 * 它不解析协议 body、不发 Server 请求、不决定命令内容；local_http_server 和 voice_proxy
 * 通过它校验完整 device_id 是否允许访问。
 */

#include "child_registry.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_config.h"

static const char *TAG = "child_registry";

static child_registry_entry_t s_entries[GATEWAY_CONFIG_MAX_CHILDREN];
static SemaphoreHandle_t s_lock;
static bool s_initialized;

static child_registry_status_t refresh_status_locked(child_registry_entry_t *entry,
                                                     int64_t timestamp_ms);

static bool peer_mac_is_valid(const uint8_t peer_mac[CHILD_REGISTRY_PEER_MAC_LEN])
{
    if (peer_mac == NULL || (peer_mac[0] & 0x01U) != 0U) {
        return false;
    }

    bool any_nonzero = false;
    for (size_t i = 0; i < CHILD_REGISTRY_PEER_MAC_LEN; ++i) {
        any_nonzero = any_nonzero || peer_mac[i] != 0U;
    }
    return any_nonzero;
}

static bool status_is_onlineish(child_registry_status_t status)
{
    return status == CHILD_REGISTRY_STATUS_ONLINE ||
           status == CHILD_REGISTRY_STATUS_VOICE_BUSY ||
           status == CHILD_REGISTRY_STATUS_LINK_LOST;
}

static const char *offline_reason_locked(const child_registry_entry_t *entry,
                                         child_registry_status_t status)
{
    if (entry == NULL || !entry->registered) {
        return "never_seen";
    }
    if (status != CHILD_REGISTRY_STATUS_OFFLINE) {
        return NULL;
    }
    return entry->link_lost_since_ms > 0 ? "link_lost_grace_expired" : "heartbeat_timeout";
}

static void set_default_status_view(child_registry_status_view_t *view)
{
    if (view == NULL) {
        return;
    }

    *view = (child_registry_status_view_t){
        .registered = false,
        .online = false,
        .status = CHILD_REGISTRY_STATUS_OFFLINE,
        .last_seen_ms = 0,
        .link_lost = false,
        .voice_busy = false,
        .offline_reason = "never_seen",
    };
}

const char *child_registry_status_name(child_registry_status_t status)
{
    switch (status) {
    case CHILD_REGISTRY_STATUS_ONLINE:
        return "online";
    case CHILD_REGISTRY_STATUS_VOICE_BUSY:
        return "voice_busy";
    case CHILD_REGISTRY_STATUS_LINK_LOST:
        return "link_lost";
    case CHILD_REGISTRY_STATUS_OFFLINE:
    default:
        return "offline";
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static child_registry_entry_t *find_locked(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; i++) {
        if (s_entries[i].device_id[0] != '\0' &&
            strcmp(s_entries[i].device_id, device_id) == 0) {
            return &s_entries[i];
        }
    }

    return NULL;
}

static child_registry_entry_t *find_or_allocate_locked(const char *device_id)
{
    if (device_id == NULL || strlen(device_id) >= CHILD_REGISTRY_DEVICE_ID_LEN) {
        ESP_LOGW(TAG, "child registry rejected invalid device_id length");
        return NULL;
    }

    child_registry_entry_t *entry = find_locked(device_id);
    if (entry != NULL) {
        return entry;
    }

    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; i++) {
        if (s_entries[i].device_id[0] == '\0') {
            strlcpy(s_entries[i].device_id, device_id, sizeof(s_entries[i].device_id));
            return &s_entries[i];
        }
    }

    ESP_LOGE(TAG,
             "child registry full device_id=%s max_children=%u",
             device_id,
             (unsigned int)GATEWAY_CONFIG_MAX_CHILDREN);
    return NULL;
}

esp_err_t child_registry_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_entries, 0, sizeof(s_entries));
    s_initialized = true;
    ESP_LOGI(TAG, "registry initialized allowlist_count=%u",
             (unsigned int)gateway_config_get()->children_allowlist_count);
    return ESP_OK;
}

bool child_registry_is_allowed(const char *device_id)
{
    return gateway_config_child_allowed(device_id);
}

esp_err_t child_registry_register_or_update(const char *device_id,
                                            const char *room_id,
                                            const char *alias,
                                            const char *capabilities,
                                            uint32_t seq)
{
    if (!child_registry_is_allowed(device_id)) {
        ESP_LOGW(TAG,
                 "child register rejected device_id=%s reason=not_allowed",
                 device_id != NULL ? device_id : "<null>");
        return ESP_ERR_NOT_ALLOWED;
    }

    if (s_lock == NULL) {
        ESP_LOGW(TAG, "child register rejected device_id=%s reason=not_initialized", device_id);
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_or_allocate_locked(device_id);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    if (room_id != NULL) {
        strlcpy(entry->room_id, room_id, sizeof(entry->room_id));
    }
    if (alias != NULL) {
        strlcpy(entry->alias, alias, sizeof(entry->alias));
    }
    if (capabilities != NULL) {
        strlcpy(entry->capabilities, capabilities, sizeof(entry->capabilities));
    }

    entry->last_seq = seq;
    entry->last_seen_ms = now_ms();
    entry->link_lost_since_ms = 0;
    entry->status = CHILD_REGISTRY_STATUS_ONLINE;
    entry->registered = true;
    entry->online = true;
    if (entry->peer_ip[0] != '\0') {
        ESP_LOGD(TAG, "child peer_ip=%s device_id=%s", entry->peer_ip, device_id);
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "child registered/updated device_id=%s room_id=%s alias=%s status=%s",
             device_id,
             room_id != NULL ? room_id : "",
             alias != NULL ? alias : "",
             child_registry_status_name(CHILD_REGISTRY_STATUS_ONLINE));
    return ESP_OK;
}

esp_err_t child_registry_touch(const char *device_id, uint32_t seq)
{
    if (!child_registry_is_allowed(device_id)) {
        ESP_LOGW(TAG,
                 "child touch rejected device_id=%s reason=not_allowed",
                 device_id != NULL ? device_id : "<null>");
        return ESP_ERR_NOT_ALLOWED;
    }

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_or_allocate_locked(device_id);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    entry->last_seq = seq;
    entry->last_seen_ms = now_ms();
    entry->link_lost_since_ms = 0;
    /* A validated heartbeat/sensor/CSI identity is sufficient to own a session. */
    entry->registered = true;
    if (entry->status != CHILD_REGISTRY_STATUS_VOICE_BUSY) {
        entry->status = CHILD_REGISTRY_STATUS_ONLINE;
    }
    entry->online = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t child_registry_confirm_identity(const char *device_id)
{
    if (!child_registry_is_allowed(device_id)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_or_allocate_locked(device_id);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    entry->last_seen_ms = now_ms();
    entry->link_lost_since_ms = 0;
    entry->registered = true;
    if (entry->status != CHILD_REGISTRY_STATUS_VOICE_BUSY) {
        entry->status = CHILD_REGISTRY_STATUS_ONLINE;
    }
    entry->online = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t child_registry_note_activity(const char *device_id, uint32_t seq)
{
    if (!child_registry_is_allowed(device_id)) {
        ESP_LOGW(TAG,
                 "child activity rejected device_id=%s reason=not_allowed",
                 device_id != NULL ? device_id : "<null>");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_or_allocate_locked(device_id);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    (void)refresh_status_locked(entry, now_ms());
    entry->last_seq = seq;
    /* Passive status must neither clear nor extend link_lost/offline/heartbeat identity. */
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t child_registry_update_peer_ip(const char *device_id, const char *peer_ip)
{
    if (!child_registry_is_allowed(device_id)) {
        ESP_LOGW(TAG,
                 "peer ip update rejected device_id=%s reason=not_allowed",
                 device_id != NULL ? device_id : "<null>");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (peer_ip == NULL || peer_ip[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(peer_ip) >= sizeof(((child_registry_entry_t *)0)->peer_ip)) {
        ESP_LOGW(TAG,
                 "peer ip update rejected device_id=%s peer_ip=%s",
                 device_id,
                 peer_ip);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_or_allocate_locked(device_id);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    if (strcmp(entry->peer_ip, peer_ip) != 0) {
        for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
            child_registry_entry_t *other = &s_entries[i];
            if (other != entry && strcmp(other->peer_ip, peer_ip) == 0) {
                other->peer_ip[0] = '\0';
            }
        }
        ESP_LOGI(TAG, "child peer mapped device_id=%s peer_ip=%s", device_id, peer_ip);
        strlcpy(entry->peer_ip, peer_ip, sizeof(entry->peer_ip));
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool child_registry_get_peer_ip(const char *device_id, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';
    if (!child_registry_is_allowed(device_id) || s_lock == NULL) {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_locked(device_id);
    if (entry != NULL && entry->peer_ip[0] != '\0') {
        strlcpy(out, entry->peer_ip, out_size);
        found = true;
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool child_registry_find_device_by_peer_ip(const char *peer_ip,
                                           char *out_device_id,
                                           size_t out_size)
{
    if (out_device_id == NULL || out_size == 0U) {
        return false;
    }
    out_device_id[0] = '\0';
    if (peer_ip == NULL || peer_ip[0] == '\0' || s_lock == NULL) {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        const child_registry_entry_t *entry = &s_entries[i];
        if (entry->device_id[0] == '\0' || entry->peer_ip[0] == '\0' ||
            strcmp(entry->peer_ip, peer_ip) != 0) {
            continue;
        }
        strlcpy(out_device_id, entry->device_id, out_size);
        found = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return found;
}

esp_err_t child_registry_update_peer_mac(
    const char *device_id,
    const uint8_t peer_mac[CHILD_REGISTRY_PEER_MAC_LEN])
{
    if (!child_registry_is_allowed(device_id)) {
        ESP_LOGW(TAG,
                 "peer mac update rejected device_id=%s reason=not_allowed",
                 device_id != NULL ? device_id : "<null>");
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!peer_mac_is_valid(peer_mac)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_or_allocate_locked(device_id);
    if (entry == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        child_registry_entry_t *other = &s_entries[i];
        if (other != entry && other->peer_mac_valid &&
            memcmp(other->peer_mac, peer_mac, CHILD_REGISTRY_PEER_MAC_LEN) == 0) {
            memset(other->peer_mac, 0, sizeof(other->peer_mac));
            other->peer_mac_valid = false;
        }
    }
    if (!entry->peer_mac_valid ||
        memcmp(entry->peer_mac, peer_mac, CHILD_REGISTRY_PEER_MAC_LEN) != 0) {
        memcpy(entry->peer_mac, peer_mac, CHILD_REGISTRY_PEER_MAC_LEN);
        entry->peer_mac_valid = true;
        changed = true;
    }
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "child peer mac mapped device_id=%s mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 device_id,
                 peer_mac[0],
                 peer_mac[1],
                 peer_mac[2],
                 peer_mac[3],
                 peer_mac[4],
                 peer_mac[5]);
    }
    return ESP_OK;
}

bool child_registry_find_device_by_peer_mac(
    const uint8_t peer_mac[CHILD_REGISTRY_PEER_MAC_LEN],
    char *out_device_id,
    size_t out_size)
{
    if (out_device_id == NULL || out_size == 0U) {
        return false;
    }
    out_device_id[0] = '\0';
    if (!peer_mac_is_valid(peer_mac) || s_lock == NULL) {
        return false;
    }

    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        const child_registry_entry_t *entry = &s_entries[i];
        if (entry->device_id[0] == '\0' || !entry->peer_mac_valid ||
            memcmp(entry->peer_mac, peer_mac, CHILD_REGISTRY_PEER_MAC_LEN) != 0) {
            continue;
        }
        strlcpy(out_device_id, entry->device_id, out_size);
        found = true;
        break;
    }
    xSemaphoreGive(s_lock);
    return found;
}

static bool mark_link_lost_locked(child_registry_entry_t *entry, int64_t timestamp_ms)
{
    if (entry == NULL || entry->device_id[0] == '\0' || !entry->registered) {
        return false;
    }

    const bool changed = entry->status != CHILD_REGISTRY_STATUS_LINK_LOST;
    entry->status = CHILD_REGISTRY_STATUS_LINK_LOST;
    entry->online = true;
    if (timestamp_ms > entry->link_lost_since_ms) {
        entry->link_lost_since_ms = timestamp_ms;
    }
    return changed;
}

void child_registry_mark_link_lost(const char *device_id, const char *reason)
{
    child_registry_mark_link_lost_at(device_id, now_ms(), reason);
}

void child_registry_mark_link_lost_at(const char *device_id,
                                      int64_t disconnected_at_ms,
                                      const char *reason)
{
    if (!child_registry_is_allowed(device_id) || s_lock == NULL) {
        return;
    }

    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = mark_link_lost_locked(find_locked(device_id),
                                    disconnected_at_ms > 0 ? disconnected_at_ms : now_ms());
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG,
                 "child registry link_lost device_id=%s reason=%s grace_ms=%u",
                 device_id,
                 reason != NULL ? reason : "<none>",
                 (unsigned int)gateway_config_get()->link_lost_grace_ms);
    }
}

void child_registry_mark_all_link_lost(const char *reason)
{
    if (s_lock == NULL) {
        return;
    }

    const int64_t timestamp_ms = now_ms();
    size_t changed_count = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; i++) {
        /*
         * AP_STADISCONNECTED 只能说明 WiFi 层断开，C5 可能在宽限期内重连并幂等 register。
         * 因此先标 link_lost，不删除 entry，超过 grace 后才转 offline。
         */
        if (mark_link_lost_locked(&s_entries[i], timestamp_ms)) {
            ++changed_count;
        }
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG,
             "child registry link_lost grace start reason=%s grace_ms=%u changed=%u",
             reason != NULL ? reason : "<none>",
             (unsigned int)gateway_config_get()->link_lost_grace_ms,
             (unsigned int)changed_count);
}

void child_registry_set_voice_busy(const char *device_id, bool busy)
{
    if (!child_registry_is_allowed(device_id) || s_lock == NULL) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_or_allocate_locked(device_id);
    if (entry != NULL) {
        const int64_t timestamp_ms = now_ms();
        const child_registry_status_t status = refresh_status_locked(entry, timestamp_ms);
        if (status != CHILD_REGISTRY_STATUS_LINK_LOST &&
            status != CHILD_REGISTRY_STATUS_OFFLINE) {
            entry->last_seen_ms = timestamp_ms;
            entry->link_lost_since_ms = 0;
            entry->online = true;
            entry->status = busy ? CHILD_REGISTRY_STATUS_VOICE_BUSY :
                                   CHILD_REGISTRY_STATUS_ONLINE;
        }
    }
    xSemaphoreGive(s_lock);
}

static child_registry_status_t refresh_status_locked(child_registry_entry_t *entry, int64_t timestamp_ms)
{
    if (entry == NULL || !entry->registered) {
        return CHILD_REGISTRY_STATUS_OFFLINE;
    }

    if (entry->status == CHILD_REGISTRY_STATUS_VOICE_BUSY) {
        /*
         * voice_busy 期间 C5 会暂停普通 heartbeat/status/command poll；
         * 普通心跳缺失不能据此判 offline，否则长语音回合会造成状态抖动。
         */
        entry->online = true;
        return entry->status;
    }

    if (entry->status == CHILD_REGISTRY_STATUS_LINK_LOST) {
        int64_t grace_ms = (int64_t)gateway_config_get()->link_lost_grace_ms;
        if (entry->link_lost_since_ms > 0 &&
            timestamp_ms - entry->link_lost_since_ms <= grace_ms) {
            entry->online = true;
            return entry->status;
        }
        entry->status = CHILD_REGISTRY_STATUS_OFFLINE;
        entry->online = false;
        return entry->status;
    }

    if (entry->status == CHILD_REGISTRY_STATUS_OFFLINE &&
        entry->link_lost_since_ms > 0) {
        entry->online = false;
        return entry->status;
    }

    const int64_t timeout_ms = (int64_t)gateway_config_get()->heartbeat_timeout_ms;
    if (entry->last_seen_ms <= 0 || timestamp_ms - entry->last_seen_ms > timeout_ms) {
        entry->status = CHILD_REGISTRY_STATUS_OFFLINE;
        entry->online = false;
        return entry->status;
    }

    entry->status = CHILD_REGISTRY_STATUS_ONLINE;
    entry->online = true;
    return entry->status;
}

bool child_registry_is_online(const char *device_id)
{
    bool online = false;

    if (s_lock == NULL) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_locked(device_id);
    if (entry != NULL) {
        child_registry_status_t status = refresh_status_locked(entry, now_ms());
        online = status == CHILD_REGISTRY_STATUS_ONLINE ||
                 status == CHILD_REGISTRY_STATUS_VOICE_BUSY ||
                 status == CHILD_REGISTRY_STATUS_LINK_LOST;
    }
    xSemaphoreGive(s_lock);
    return online;
}

bool child_registry_refresh_status_change(const char *device_id,
                                         child_registry_status_t *out_status,
                                         child_registry_status_t *out_previous_status)
{
    child_registry_status_t previous = CHILD_REGISTRY_STATUS_OFFLINE;
    child_registry_status_t current = CHILD_REGISTRY_STATUS_OFFLINE;
    bool found = false;

    if (s_lock == NULL) {
        if (out_status != NULL) {
            *out_status = current;
        }
        if (out_previous_status != NULL) {
            *out_previous_status = previous;
        }
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_locked(device_id);
    if (entry != NULL) {
        previous = entry->status;
        current = refresh_status_locked(entry, now_ms());
        found = true;
    }
    xSemaphoreGive(s_lock);

    if (out_status != NULL) {
        *out_status = current;
    }
    if (out_previous_status != NULL) {
        *out_previous_status = previous;
    }
    return found && previous != current;
}

child_registry_status_t child_registry_get_status(const char *device_id)
{
    child_registry_status_t status = CHILD_REGISTRY_STATUS_OFFLINE;

    if (s_lock == NULL) {
        return status;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_locked(device_id);
    if (entry != NULL) {
        status = refresh_status_locked(entry, now_ms());
    }
    xSemaphoreGive(s_lock);
    return status;
}

bool child_registry_get_status_info(const char *device_id, child_registry_status_t *out_status)
{
    child_registry_status_t status = CHILD_REGISTRY_STATUS_OFFLINE;
    bool found = false;

    if (s_lock == NULL) {
        if (out_status != NULL) {
            *out_status = status;
        }
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_locked(device_id);
    if (entry != NULL) {
        status = refresh_status_locked(entry, now_ms());
        found = true;
    }
    xSemaphoreGive(s_lock);

    if (out_status != NULL) {
        *out_status = status;
    }
    return found;
}

bool child_registry_get_status_view(const char *device_id,
                                    child_registry_status_view_t *out_view)
{
    child_registry_status_view_t view;
    set_default_status_view(&view);

    if (s_lock == NULL) {
        if (out_view != NULL) {
            *out_view = view;
        }
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    child_registry_entry_t *entry = find_locked(device_id);
    if (entry != NULL) {
        const child_registry_status_t status = refresh_status_locked(entry, now_ms());
        view.registered = entry->registered;
        view.online = status_is_onlineish(status);
        view.status = status;
        view.last_seen_ms = entry->last_seen_ms;
        view.link_lost = status == CHILD_REGISTRY_STATUS_LINK_LOST;
        view.voice_busy = status == CHILD_REGISTRY_STATUS_VOICE_BUSY;
        view.offline_reason = offline_reason_locked(entry, status);
    }
    xSemaphoreGive(s_lock);

    if (out_view != NULL) {
        *out_view = view;
    }
    return view.registered;
}

size_t child_registry_snapshot(child_registry_entry_t *entries, size_t entry_count)
{
    if (entries == NULL || entry_count == 0 || s_lock == NULL) {
        return 0;
    }

    size_t copied = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN && copied < entry_count; i++) {
        if (s_entries[i].device_id[0] != '\0') {
            (void)refresh_status_locked(&s_entries[i], now_ms());
            entries[copied++] = s_entries[i];
        }
    }
    xSemaphoreGive(s_lock);
    return copied;
}
