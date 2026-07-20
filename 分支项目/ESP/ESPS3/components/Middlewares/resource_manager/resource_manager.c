/**
 * @file resource_manager.c
 * @brief ESPS3 device-level C5 live resource lifecycle manager.
 */

#include "resource_manager.h"

#include <stdio.h>
#include <string.h>

#include "child_registry.h"
#include "command_router.h"
#include "csi_placeholder_gateway.h"
#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gateway_config.h"
#include "gateway_wifi.h"
#include "network_worker.h"
#include "sensor_aggregator.h"
#include "server_client.h"

static const char *TAG = "resource_manager";

#define RESOURCE_MANAGER_PEER_IP_MISSING_LOG_MS 5000LL

typedef struct {
    bool used;
    char device_id[RESOURCE_MANAGER_DEVICE_ID_LEN];
    resource_manager_session_state_t state;
    int64_t state_since_ms;
    int64_t grace_started_ms;
    int64_t restore_not_before_us;
    int64_t last_identity_observed_us;
    int64_t last_peer_ip_missing_log_ms;
    uint32_t generation;
    bool release_pending;
    bool live_resources_ready;
} resource_session_t;

static resource_session_t s_sessions[GATEWAY_CONFIG_MAX_CHILDREN];
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_operation_lock;
static bool s_initialized;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

const char *resource_manager_session_state_name(resource_manager_session_state_t state)
{
    switch (state) {
    case RESOURCE_MANAGER_SESSION_ACTIVE:
        return "ACTIVE";
    case RESOURCE_MANAGER_SESSION_GRACE:
        return "GRACE";
    case RESOURCE_MANAGER_SESSION_RELEASED:
        return "RELEASED";
    case RESOURCE_MANAGER_SESSION_RESTORING:
        return "RESTORING";
    default:
        return "UNKNOWN";
    }
}

const char *resource_manager_identity_signal_name(resource_manager_identity_signal_t signal)
{
    switch (signal) {
    case RESOURCE_MANAGER_SIGNAL_REGISTER:
        return "register";
    case RESOURCE_MANAGER_SIGNAL_HEARTBEAT:
        return "heartbeat";
    case RESOURCE_MANAGER_SIGNAL_STATUS:
        return "status";
    case RESOURCE_MANAGER_SIGNAL_SENSOR:
        return "sensor";
    case RESOURCE_MANAGER_SIGNAL_CSI:
        return "csi";
    default:
        return "unknown";
    }
}

static resource_session_t *find_locked(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        if (s_sessions[i].used && strcmp(s_sessions[i].device_id, device_id) == 0) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static void log_state_change(const char *device_id,
                             resource_manager_session_state_t previous,
                             resource_manager_session_state_t current,
                             const char *reason,
                             uint32_t generation)
{
    ESP_LOGI(TAG,
             "SESSION_STATE_CHANGE device_id=%s from=%s to=%s reason=%s generation=%lu",
             device_id,
             resource_manager_session_state_name(previous),
             resource_manager_session_state_name(current),
             reason != NULL ? reason : "unknown",
             (unsigned long)generation);
}

static void log_restore_identity(const char *device_id,
                                 const char *peer_ip,
                                 resource_manager_session_state_t previous,
                                 resource_manager_session_state_t current,
                                 const char *reason,
                                 uint32_t generation)
{
    child_registry_entry_t entries[GATEWAY_CONFIG_MAX_CHILDREN] = {0};
    const size_t entry_count = child_registry_snapshot(entries, GATEWAY_CONFIG_MAX_CHILDREN);
    const child_registry_entry_t *entry = NULL;
    for (size_t i = 0; i < entry_count; ++i) {
        if (strcmp(entries[i].device_id, device_id) == 0) {
            entry = &entries[i];
            break;
        }
    }

    char mac[18] = "-";
    if (entry != NULL && entry->peer_mac_valid) {
        (void)snprintf(mac,
                       sizeof(mac),
                       "%02x:%02x:%02x:%02x:%02x:%02x",
                       entry->peer_mac[0],
                       entry->peer_mac[1],
                       entry->peer_mac[2],
                       entry->peer_mac[3],
                       entry->peer_mac[4],
                       entry->peer_mac[5]);
    } else if (peer_ip != NULL && peer_ip[0] != '\0') {
        uint8_t peer_mac[CHILD_REGISTRY_PEER_MAC_LEN] = {0};
        if (gateway_wifi_get_ap_client_mac(peer_ip, peer_mac)) {
            (void)snprintf(mac,
                           sizeof(mac),
                           "%02x:%02x:%02x:%02x:%02x:%02x",
                           peer_mac[0],
                           peer_mac[1],
                           peer_mac[2],
                           peer_mac[3],
                           peer_mac[4],
                           peer_mac[5]);
        }
    }

    const char *effective_ip = peer_ip != NULL && peer_ip[0] != '\0' ?
                                   peer_ip :
                                   entry != NULL && entry->peer_ip[0] != '\0' ?
                                       entry->peer_ip : "-";
    ESP_LOGI(TAG,
             "RESOURCE_RESTORE_IDENTITY device_id=%s mac=%s ip=%s old_state=%s new_state=%s restore_reason=%s generation=%lu peer_ip_present=%d",
             device_id != NULL ? device_id : "-",
             mac,
             effective_ip,
             resource_manager_session_state_name(previous),
             resource_manager_session_state_name(current),
             reason != NULL ? reason : "unknown",
             (unsigned long)generation,
             peer_ip != NULL && peer_ip[0] != '\0' ? 1 : 0);
}

static const char *link_id_for_device_id(const char *device_id)
{
    if (device_id == NULL) {
        return "-";
    }
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0 ||
        strcmp(device_id, "C51") == 0) {
        return "S3_TO_C51";
    }
    if (strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0 ||
        strcmp(device_id, "C52") == 0) {
        return "S3_TO_C52";
    }
    return "-";
}

static void log_session_diag(const resource_session_t *session,
                             const char *action,
                             const char *reason,
                             int64_t timestamp_ms)
{
    if (session == NULL) {
        return;
    }

    const int64_t now = timestamp_ms > 0 ? timestamp_ms : now_ms();
    child_registry_status_view_t view = {0};
    (void)child_registry_get_status_view(session->device_id, &view);
    const int64_t last_seen_ms = view.last_seen_ms;
    int64_t age_ms = -1;
    if (last_seen_ms > 0) {
        age_ms = now >= last_seen_ms ? now - last_seen_ms : 0;
    }

    int64_t state_age_ms = -1;
    if (session->state_since_ms > 0) {
        state_age_ms = now >= session->state_since_ms ? now - session->state_since_ms : 0;
    }

    ESP_LOGI(TAG,
             "SESSION_DIAG action=%s reason=%s device_id=%s link_id=%s session_state=%s last_seen_ms=%lld age_ms=%lld resource_generation=%lu state_age_ms=%lld",
             action != NULL ? action : "unknown",
             reason != NULL ? reason : "unknown",
             session->device_id,
             link_id_for_device_id(session->device_id),
             resource_manager_session_state_name(session->state),
             (long long)last_seen_ms,
             (long long)age_ms,
             (unsigned long)session->generation,
             (long long)state_age_ms);
}

static void transition_locked(resource_session_t *session,
                              resource_manager_session_state_t next,
                              int64_t timestamp_ms,
                              const char *reason)
{
    if (session == NULL || session->state == next) {
        return;
    }
    resource_manager_session_state_t previous = session->state;
    session->state = next;
    session->state_since_ms = timestamp_ms > 0 ? timestamp_ms : now_ms();
    log_state_change(session->device_id, previous, next, reason, session->generation);
    log_session_diag(session, "state_change", reason, session->state_since_ms);
}

static bool signal_valid(resource_manager_identity_signal_t signal)
{
    return signal >= RESOURCE_MANAGER_SIGNAL_REGISTER &&
           signal <= RESOURCE_MANAGER_SIGNAL_CSI;
}

static bool signal_can_activate(resource_manager_identity_signal_t signal)
{
    /* A protocol-validated CSI result is a valid C5 liveness/identity event. */
    return signal >= RESOURCE_MANAGER_SIGNAL_REGISTER &&
           signal <= RESOURCE_MANAGER_SIGNAL_CSI;
}

static bool session_has_live_resources(const resource_session_t *session)
{
    if (session == NULL) {
        return false;
    }
    return session->state == RESOURCE_MANAGER_SESSION_ACTIVE ||
           (session->state == RESOURCE_MANAGER_SESSION_RESTORING &&
            session->live_resources_ready);
}

static const char *resource_release_reason_label(const char *reason)
{
    if (reason == NULL || reason[0] == '\0') {
        return "UNKNOWN";
    }
    if (strcmp(reason, "ap_sta_disconnected") == 0) {
        return "STA_DISCONNECT";
    }
    if (strcmp(reason, "softap_stop") == 0) {
        return "SOFTAP_STOP";
    }
    if (strcmp(reason, "gateway_reset") == 0) {
        return "GATEWAY_RESET";
    }
    if (strcmp(reason, "global_resource_reset") == 0) {
        return "GLOBAL_RESOURCE_RESET";
    }
    if (strcmp(reason, "heartbeat_timeout") == 0) {
        return "HEARTBEAT_TIMEOUT";
    }
    if (strcmp(reason, "register_update_failed") == 0) {
        return "REGISTER_UPDATE_FAILED";
    }
    if (strcmp(reason, "release_retry") == 0) {
        return "RELEASE_RETRY";
    }
    return reason;
}

static bool global_release_reason_allowed(const char *reason)
{
    return reason != NULL &&
           (strcmp(reason, "softap_stop") == 0 ||
            strcmp(reason, "gateway_reset") == 0 ||
            strcmp(reason, "global_resource_reset") == 0);
}

static esp_err_t release_live_resources(const char *device_id,
                                        int64_t cutoff_us,
                                        const char *reason)
{
    esp_err_t command_ret = command_router_suspend_peer(device_id);
    esp_err_t sensor_ret = sensor_aggregator_suspend_peer(device_id);
    esp_err_t http_ret = server_client_cancel_peer_requests(device_id);
    esp_err_t csi_ret = csi_gateway_suspend_peer_at_us(device_id, cutoff_us);
    esp_err_t queue_ret = network_worker_release_peer_resources(device_id);

    ESP_LOGI(TAG,
             "RESOURCE_RELEASE_REASON=%s device=%s reason=%s cutoff_us=%lld csi=%s command=%s sensor=%s http=%s queue=%s",
             resource_release_reason_label(reason),
             device_id,
             reason != NULL ? reason : "unknown",
             (long long)cutoff_us,
             esp_err_to_name(csi_ret),
             esp_err_to_name(command_ret),
             esp_err_to_name(sensor_ret),
             esp_err_to_name(http_ret),
             esp_err_to_name(queue_ret));

    if (http_ret != ESP_OK) {
        return http_ret;
    }
    if (csi_ret != ESP_OK) {
        return csi_ret;
    }
    if (command_ret != ESP_OK) {
        return command_ret;
    }
    if (sensor_ret != ESP_OK) {
        return sensor_ret;
    }
    return queue_ret;
}

static esp_err_t restore_live_resources(const char *device_id, const char *reason)
{
    esp_err_t command_ret = command_router_restore_peer(device_id);
    esp_err_t sensor_ret = command_ret == ESP_OK ? sensor_aggregator_restore_peer(device_id) :
                                                   command_ret;
    esp_err_t csi_ret = sensor_ret == ESP_OK ? csi_gateway_restore_peer(device_id) : sensor_ret;
    esp_err_t queue_ret = csi_ret == ESP_OK ? network_worker_restore_peer_resources(device_id) :
                                             csi_ret;

    if (command_ret != ESP_OK || sensor_ret != ESP_OK || csi_ret != ESP_OK ||
        queue_ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "RESOURCE_RESTORE device_id=%s reason=%s status=failed command=%s sensor=%s csi=%s queue=%s",
                 device_id,
                 reason != NULL ? reason : "unknown",
                 esp_err_to_name(command_ret),
                 esp_err_to_name(sensor_ret),
                 esp_err_to_name(csi_ret),
                 esp_err_to_name(queue_ret));
        return command_ret != ESP_OK ? command_ret :
               sensor_ret != ESP_OK ? sensor_ret :
               csi_ret != ESP_OK ? csi_ret : queue_ret;
    }

    ESP_LOGI(TAG,
             "RESOURCE_RESTORE device_id=%s reason=%s status=restoring command=restored sensor=restored csi=warmup queue=ready",
             device_id,
             reason != NULL ? reason : "unknown");
    return ESP_OK;
}

static void log_restore_transition(const char *device_id,
                                   resource_manager_session_state_t previous,
                                   resource_manager_session_state_t current,
                                   const char *reason,
                                   uint32_t generation)
{
    ESP_LOGI(TAG,
             "RESOURCE_RESTORE_STATE device_id=%s from=%s to=%s reason=%s generation=%lu",
             device_id,
             resource_manager_session_state_name(previous),
             resource_manager_session_state_name(current),
             reason != NULL ? reason : "unknown",
             (unsigned long)generation);
}

static void log_restore_failure(const char *device_id,
                                const char *reason,
                                const char *stage,
                                esp_err_t ret,
                                uint32_t generation)
{
    ESP_LOGW(TAG,
             "RESOURCE_RESTORE_FAILED device_id=%s reason=%s stage=%s ret=%s generation=%lu",
             device_id,
             reason != NULL ? reason : "unknown",
             stage != NULL ? stage : "unknown",
             esp_err_to_name(ret),
             (unsigned long)generation);
}

esp_err_t resource_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_operation_lock == NULL) {
        s_operation_lock = xSemaphoreCreateMutex();
        if (s_operation_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const gateway_runtime_config_t *config = gateway_config_get();
    const int64_t timestamp_us = now_us();
    const int64_t timestamp_ms = timestamp_us / 1000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_sessions, 0, sizeof(s_sessions));
    size_t count = config->children_allowlist_count;
    if (count > GATEWAY_CONFIG_MAX_CHILDREN) {
        count = GATEWAY_CONFIG_MAX_CHILDREN;
    }
    for (size_t i = 0; i < count; ++i) {
        if (config->children_allowlist[i] == NULL || config->children_allowlist[i][0] == '\0') {
            continue;
        }
        s_sessions[i].used = true;
        strlcpy(s_sessions[i].device_id,
                config->children_allowlist[i],
                sizeof(s_sessions[i].device_id));
        s_sessions[i].state = RESOURCE_MANAGER_SESSION_RELEASED;
        s_sessions[i].state_since_ms = timestamp_ms;
        s_sessions[i].restore_not_before_us = timestamp_us;
        s_sessions[i].generation = 1U;
    }
    s_initialized = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG,
             "resource manager initialized sessions=%u initial_state=RELEASED grace_ms=%u heartbeat_timeout_ms=%u",
             (unsigned int)count,
             (unsigned int)config->link_lost_grace_ms,
             (unsigned int)config->heartbeat_timeout_ms);
    return ESP_OK;
}

static esp_err_t release_peer_to(const char *device_id,
                                 int64_t released_at_ms,
                                 int64_t released_at_us,
                                 const char *reason,
                                 uint32_t expected_generation,
                                 resource_manager_session_state_t target)
{
    if (!gateway_config_child_allowed(device_id) || s_lock == NULL ||
        s_operation_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int64_t timestamp_us = released_at_us > 0 ?
                                     released_at_us :
                                     released_at_ms > 0 ? released_at_ms * 1000 : now_us();
    const int64_t timestamp_ms = released_at_ms > 0 ? released_at_ms : timestamp_us / 1000;
    bool should_release = false;
    bool should_mark_link_lost = false;
    esp_err_t result = ESP_OK;

    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    resource_session_t *session = find_locked(device_id);
    if (session == NULL) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_generation != 0U && session->generation != expected_generation) {
        uint32_t current_generation = session->generation;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        ESP_LOGW(TAG,
                 "RESOURCE_RELEASE_SKIP reason=STALE_SESSION device=%s expected_session=%lu current_session=%lu release_reason=%s",
                 device_id,
                 (unsigned long)expected_generation,
                 (unsigned long)current_generation,
                 reason != NULL ? reason : "unknown");
        return ESP_ERR_INVALID_STATE;
    }
    if ((session->last_identity_observed_us > 0 &&
         timestamp_us < session->last_identity_observed_us) ||
        timestamp_us < session->restore_not_before_us) {
        int64_t last_identity_us = session->last_identity_observed_us;
        int64_t current_cutoff_us = session->restore_not_before_us;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        ESP_LOGI(TAG,
                 "resource release ignored device_id=%s reason=%s disconnect_us=%lld last_identity_us=%lld cutoff_us=%lld decision=stale_event",
                 device_id,
                 reason != NULL ? reason : "unknown",
                 (long long)timestamp_us,
                 (long long)last_identity_us,
                 (long long)current_cutoff_us);
        return ESP_OK;
    }
    if (target == RESOURCE_MANAGER_SESSION_GRACE) {
        /* Every disconnect advances the cutoff and must prune the newly stale ingress. */
        should_release = true;
        ++session->generation;
        if (session->generation == 0U) {
            session->generation = 1U;
        }
        session->restore_not_before_us = timestamp_us;
        session->grace_started_ms = timestamp_ms;
        session->release_pending = true;
        session->live_resources_ready = false;
        transition_locked(session, target, timestamp_ms, reason);
        should_mark_link_lost = true;
    } else if (session->state == RESOURCE_MANAGER_SESSION_GRACE &&
               target == RESOURCE_MANAGER_SESSION_RELEASED) {
        session->grace_started_ms = 0;
        session->live_resources_ready = false;
        transition_locked(session, target, timestamp_ms, reason);
    } else if (session_has_live_resources(session) &&
               target == RESOURCE_MANAGER_SESSION_RELEASED) {
        should_release = true;
        session->release_pending = true;
        ++session->generation;
        if (session->generation == 0U) {
            session->generation = 1U;
        }
        session->restore_not_before_us = timestamp_us;
        session->grace_started_ms = 0;
        session->live_resources_ready = false;
        transition_locked(session, target, timestamp_ms, reason);
    }
    xSemaphoreGive(s_lock);

    if (should_mark_link_lost) {
        child_registry_mark_link_lost_at(device_id, timestamp_ms, reason);
    }

    if (should_release) {
        result = release_live_resources(device_id, timestamp_us, reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        session = find_locked(device_id);
        if (session != NULL) {
            session->release_pending = result != ESP_OK;
        }
        xSemaphoreGive(s_lock);
    }
    xSemaphoreGive(s_operation_lock);
    return result;
}

esp_err_t resource_manager_release_peer(const char *device_id,
                                        int64_t disconnected_at_ms,
                                        const char *reason)
{
    return release_peer_to(device_id,
                           disconnected_at_ms,
                           disconnected_at_ms > 0 ? disconnected_at_ms * 1000 : 0,
                           reason,
                           0U,
                           RESOURCE_MANAGER_SESSION_GRACE);
}

esp_err_t resource_manager_release_peer_at_us(const char *device_id,
                                              int64_t disconnected_at_us,
                                              const char *reason)
{
    return release_peer_to(device_id,
                           disconnected_at_us > 0 ? disconnected_at_us / 1000 : 0,
                           disconnected_at_us,
                           reason,
                           0U,
                           RESOURCE_MANAGER_SESSION_GRACE);
}

esp_err_t resource_manager_release_child_by_identity(const char *device_id,
                                                     uint32_t session_generation,
                                                     int64_t disconnected_at_us,
                                                     const char *reason)
{
    if (session_generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    return release_peer_to(device_id,
                           disconnected_at_us > 0 ? disconnected_at_us / 1000 : 0,
                           disconnected_at_us,
                           reason,
                           session_generation,
                           RESOURCE_MANAGER_SESSION_GRACE);
}

void resource_manager_release_all(int64_t disconnected_at_ms, const char *reason)
{
    char device_ids[GATEWAY_CONFIG_MAX_CHILDREN][RESOURCE_MANAGER_DEVICE_ID_LEN] = {0};
    size_t count = 0U;
    if (s_lock == NULL) {
        return;
    }
    if (!global_release_reason_allowed(reason)) {
        ESP_LOGW(TAG,
                 "RESOURCE_RELEASE_SKIP reason=GLOBAL_RELEASE_NOT_ALLOWED release_reason=%s",
                 reason != NULL ? reason : "unknown");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        if (s_sessions[i].used) {
            strlcpy(device_ids[count], s_sessions[i].device_id, sizeof(device_ids[count]));
            ++count;
        }
    }
    xSemaphoreGive(s_lock);

    ESP_LOGW(TAG,
             "RESOURCE_RELEASE_GLOBAL reason=%s count=%u scope=all",
             reason != NULL ? reason : "unknown",
             (unsigned int)count);

    for (size_t i = 0; i < count; ++i) {
        (void)resource_manager_release_peer(device_ids[i], disconnected_at_ms, reason);
    }
}

void resource_manager_release_all_at_us(int64_t disconnected_at_us, const char *reason)
{
    char device_ids[GATEWAY_CONFIG_MAX_CHILDREN][RESOURCE_MANAGER_DEVICE_ID_LEN] = {0};
    size_t count = 0U;
    if (s_lock == NULL) {
        return;
    }
    if (!global_release_reason_allowed(reason)) {
        ESP_LOGW(TAG,
                 "RESOURCE_RELEASE_SKIP reason=GLOBAL_RELEASE_NOT_ALLOWED release_reason=%s",
                 reason != NULL ? reason : "unknown");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        if (s_sessions[i].used) {
            strlcpy(device_ids[count], s_sessions[i].device_id, sizeof(device_ids[count]));
            ++count;
        }
    }
    xSemaphoreGive(s_lock);

    ESP_LOGW(TAG,
             "RESOURCE_RELEASE_GLOBAL reason=%s count=%u scope=all",
             reason != NULL ? reason : "unknown",
             (unsigned int)count);

    for (size_t i = 0; i < count; ++i) {
        (void)resource_manager_release_peer_at_us(device_ids[i], disconnected_at_us, reason);
    }
}

esp_err_t resource_manager_confirm_peer(const char *device_id,
                                        const char *peer_ip,
                                        resource_manager_identity_signal_t signal,
                                        int64_t observed_at_ms)
{
    return resource_manager_confirm_peer_at_us(device_id,
                                               peer_ip,
                                               signal,
                                               observed_at_ms > 0 ? observed_at_ms * 1000 : 0);
}

esp_err_t resource_manager_confirm_peer_at_us(const char *device_id,
                                              const char *peer_ip,
                                              resource_manager_identity_signal_t signal,
                                              int64_t observed_at_us)
{
    if (!gateway_config_child_allowed(device_id) || !signal_valid(signal) || s_lock == NULL ||
        s_operation_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t signal_us = observed_at_us > 0 ? observed_at_us : now_us();
    const char *signal_name = resource_manager_identity_signal_name(signal);
    const bool activation_signal = signal_can_activate(signal);
    const bool peer_ip_present = peer_ip != NULL && peer_ip[0] != '\0';
    uint32_t restore_generation = 0U;
    int64_t release_cutoff_us = 0;
    resource_manager_session_state_t state_before_identity = RESOURCE_MANAGER_SESSION_RELEASED;
    bool should_restore_resources = false;

    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    resource_session_t *session = find_locked(device_id);
    if (session == NULL) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (signal_us < session->restore_not_before_us) {
        int64_t not_before_us = session->restore_not_before_us;
        log_session_diag(session, "restore_rejected", "stale_ingress", signal_us / 1000);
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        ESP_LOGW(TAG,
                 "resource restore rejected device_id=%s signal=%s observed_at_us=%lld not_before_us=%lld reason=stale_ingress",
                 device_id,
                 signal_name,
                 (long long)signal_us,
                 (long long)not_before_us);
        return ESP_ERR_INVALID_STATE;
    }
    state_before_identity = session->state;
    restore_generation = session->generation;
    const int64_t timestamp_ms = signal_us / 1000;
    const bool log_peer_ip_missing = !peer_ip_present &&
                                     (session->last_peer_ip_missing_log_ms == 0 ||
                                      timestamp_ms - session->last_peer_ip_missing_log_ms >=
                                          RESOURCE_MANAGER_PEER_IP_MISSING_LOG_MS);
    if (log_peer_ip_missing) {
        session->last_peer_ip_missing_log_ms = timestamp_ms;
    }
    xSemaphoreGive(s_lock);

    if (peer_ip_present) {
        esp_err_t peer_ret = child_registry_update_peer_ip(device_id, peer_ip);
        if (peer_ret != ESP_OK) {
            log_restore_failure(device_id,
                                signal_name,
                                "peer_ip_update",
                                peer_ret,
                                restore_generation);
            xSemaphoreGive(s_operation_lock);
            return peer_ret;
        }
    } else if (log_peer_ip_missing) {
        char existing_peer_ip[16] = {0};
        const bool cached_peer_ip = child_registry_get_peer_ip(device_id,
                                                                existing_peer_ip,
                                                                sizeof(existing_peer_ip));
        ESP_LOGI(TAG,
                 "RESOURCE_RESTORE_PEER_IP device_id=%s signal=%s peer_ip_present=0 cached_peer_ip_present=%d decision=identity_allowed",
                 device_id,
                 signal_name,
                 cached_peer_ip ? 1 : 0);
    }

    esp_err_t identity_ret = child_registry_confirm_identity(device_id);
    if (identity_ret != ESP_OK) {
        log_restore_failure(device_id,
                            signal_name,
                            "identity_confirm",
                            identity_ret,
                            restore_generation);
        xSemaphoreGive(s_operation_lock);
        return identity_ret;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    session = find_locked(device_id);
    if (session == NULL || signal_us < session->restore_not_before_us) {
        if (session != NULL) {
            log_session_diag(session, "restore_rejected", "stale_after_identity", signal_us / 1000);
        }
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (signal_us > session->last_identity_observed_us) {
        session->last_identity_observed_us = signal_us;
    }

    if (session->state == RESOURCE_MANAGER_SESSION_ACTIVE) {
        session->live_resources_ready = true;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_OK;
    }

    if (session->state == RESOURCE_MANAGER_SESSION_GRACE ||
        session->state == RESOURCE_MANAGER_SESSION_RELEASED) {
        resource_manager_session_state_t previous = session->state;
        restore_generation = session->generation;
        session->live_resources_ready = false;
        transition_locked(session,
                          RESOURCE_MANAGER_SESSION_RESTORING,
                          signal_us / 1000,
                          signal_name);
        log_restore_transition(device_id,
                               previous,
                               RESOURCE_MANAGER_SESSION_RESTORING,
                               signal_name,
                               restore_generation);
        log_restore_identity(device_id,
                             peer_ip,
                             previous,
                             RESOURCE_MANAGER_SESSION_RESTORING,
                             signal_name,
                             restore_generation);
        should_restore_resources = true;
    } else if (session->state == RESOURCE_MANAGER_SESSION_RESTORING) {
        restore_generation = session->generation;
        if (!session_has_live_resources(session)) {
            should_restore_resources = true;
        } else if (activation_signal) {
            session->grace_started_ms = 0;
            session->live_resources_ready = true;
            transition_locked(session,
                              RESOURCE_MANAGER_SESSION_ACTIVE,
                              signal_us / 1000,
                              signal_name);
            log_restore_transition(device_id,
                                   RESOURCE_MANAGER_SESSION_RESTORING,
                                   RESOURCE_MANAGER_SESSION_ACTIVE,
                                   signal_name,
                                   restore_generation);
            log_restore_identity(device_id,
                                 peer_ip,
                                 RESOURCE_MANAGER_SESSION_RESTORING,
                                 RESOURCE_MANAGER_SESSION_ACTIVE,
                                 signal_name,
                                 restore_generation);
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG,
                     "RESOURCE_RESTORE device_id=%s reason=%s status=ok command=active sensor=active csi=ready",
                     device_id,
                     signal_name);
            xSemaphoreGive(s_operation_lock);
            return ESP_OK;
        } else {
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG,
                     "RESOURCE_RESTORE device_id=%s reason=%s status=restoring_wait_active csi=trigger_only",
                     device_id,
                     signal_name);
            xSemaphoreGive(s_operation_lock);
            return ESP_OK;
        }
    } else {
        log_session_diag(session, "restore_rejected", "invalid_session_state", signal_us / 1000);
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(s_lock);

    esp_err_t restore_ret = should_restore_resources ?
                                restore_live_resources(device_id, signal_name) :
                                ESP_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    session = find_locked(device_id);
    if (session != NULL) {
        release_cutoff_us = session->restore_not_before_us;
    }
    bool interrupted = session == NULL || session->generation != restore_generation ||
                       session->state != RESOURCE_MANAGER_SESSION_RESTORING;
    if (!interrupted && restore_ret == ESP_OK) {
        session->release_pending = false;
        session->grace_started_ms = 0;
        session->live_resources_ready = true;
        if (activation_signal) {
            transition_locked(session,
                              RESOURCE_MANAGER_SESSION_ACTIVE,
                              now_ms(),
                              signal_name);
            log_restore_transition(device_id,
                                   RESOURCE_MANAGER_SESSION_RESTORING,
                                   RESOURCE_MANAGER_SESSION_ACTIVE,
                                   signal_name,
                                   restore_generation);
            log_restore_identity(device_id,
                                 peer_ip,
                                 RESOURCE_MANAGER_SESSION_RESTORING,
                                 RESOURCE_MANAGER_SESSION_ACTIVE,
                                 signal_name,
                                 restore_generation);
        } else {
            ESP_LOGI(TAG,
                     "RESOURCE_RESTORE device_id=%s reason=%s status=restoring_wait_active csi=trigger_only",
                     device_id,
                     signal_name);
        }
    } else if (!interrupted && restore_ret != ESP_OK) {
        session->live_resources_ready = false;
        transition_locked(session,
                          RESOURCE_MANAGER_SESSION_RELEASED,
                          now_ms(),
                          "restore_failed");
        log_restore_failure(device_id,
                            signal_name,
                            "restore_resources",
                            restore_ret,
                            restore_generation);
    }
    xSemaphoreGive(s_lock);

    if (interrupted || restore_ret != ESP_OK) {
        esp_err_t release_ret = release_live_resources(
            device_id,
            release_cutoff_us > 0 ? release_cutoff_us : now_us(),
            interrupted ? "restore_interrupted" : "restore_failed");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        session = find_locked(device_id);
        if (session != NULL && !session_has_live_resources(session)) {
            session->live_resources_ready = false;
            session->release_pending = release_ret != ESP_OK;
        }
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        if (interrupted) {
            log_restore_failure(device_id,
                                signal_name,
                                "restore_interrupted",
                                ESP_ERR_INVALID_STATE,
                                restore_generation);
        }
        return interrupted ? ESP_ERR_INVALID_STATE : restore_ret;
    }

    ESP_LOGI(TAG,
             "RESOURCE_RESTORE device_id=%s reason=%s status=ok command=%s sensor=%s csi=%s previous=%s",
             device_id,
             signal_name,
             activation_signal ? "active" : "restored",
             activation_signal ? "active" : "restored",
             activation_signal ? "warmup" : "trigger_only",
             resource_manager_session_state_name(state_before_identity));
    xSemaphoreGive(s_operation_lock);
    return ESP_OK;
}

esp_err_t resource_manager_prepare_reconnect_at_us(const char *device_id,
                                                   int64_t observed_at_us,
                                                   const char *reason)
{
    if (!gateway_config_child_allowed(device_id) || s_lock == NULL ||
        s_operation_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t timestamp_us = observed_at_us > 0 ? observed_at_us : now_us();
    const int64_t timestamp_ms = timestamp_us / 1000;
    const char *restore_reason = reason != NULL && reason[0] != '\0' ?
                                     reason : "ap_sta_connected";

    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    resource_session_t *session = find_locked(device_id);
    if (session == NULL) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (timestamp_us < session->restore_not_before_us) {
        int64_t not_before_us = session->restore_not_before_us;
        log_session_diag(session, "restore_prepare_rejected", "stale_ap_connected", timestamp_ms);
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        ESP_LOGW(TAG,
                 "RESOURCE_RESTORE_PREP rejected device_id=%s reason=%s observed_at_us=%lld not_before_us=%lld decision=stale_event",
                 device_id,
                 restore_reason,
                 (long long)timestamp_us,
                 (long long)not_before_us);
        return ESP_ERR_INVALID_STATE;
    }

    if (session->state == RESOURCE_MANAGER_SESSION_RELEASED ||
        session->state == RESOURCE_MANAGER_SESSION_GRACE) {
        resource_manager_session_state_t previous = session->state;
        session->live_resources_ready = false;
        transition_locked(session,
                          RESOURCE_MANAGER_SESSION_RESTORING,
                          timestamp_ms,
                          restore_reason);
        log_restore_transition(device_id,
                               previous,
                               RESOURCE_MANAGER_SESSION_RESTORING,
                               restore_reason,
                               session->generation);
        ESP_LOGI(TAG,
                 "RESOURCE_RESTORE_PREP device_id=%s reason=%s status=waiting_for_identity generation=%lu",
                 device_id,
                 restore_reason,
                 (unsigned long)session->generation);
    } else {
        ESP_LOGI(TAG,
                 "RESOURCE_RESTORE_PREP device_id=%s reason=%s status=noop state=%s generation=%lu",
                 device_id,
                 restore_reason,
                 resource_manager_session_state_name(session->state),
                 (unsigned long)session->generation);
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_operation_lock);
    return ESP_OK;
}

esp_err_t resource_manager_complete_restore(const char *device_id, const char *reason)
{
    if (!gateway_config_child_allowed(device_id) || s_lock == NULL ||
        s_operation_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_operation_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    resource_session_t *session = find_locked(device_id);
    if (session == NULL) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (session->state == RESOURCE_MANAGER_SESSION_ACTIVE) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_OK;
    }
    if (session->state != RESOURCE_MANAGER_SESSION_RESTORING) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_INVALID_STATE;
    }

    const char *restore_reason = reason != NULL ? reason : "csi_warmup_complete";
    if (strcmp(restore_reason, "csi_warmup_complete") == 0) {
        session->live_resources_ready = true;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG,
                 "RESOURCE_RESTORE device_id=%s reason=%s status=restoring_wait_active csi=warmup_complete activation=deferred",
                 device_id,
                 restore_reason);
        xSemaphoreGive(s_operation_lock);
        return ESP_OK;
    }
    if (!session->live_resources_ready) {
        uint32_t generation = session->generation;
        xSemaphoreGive(s_lock);
        log_restore_failure(device_id,
                            restore_reason,
                            "complete_without_resources",
                            ESP_ERR_INVALID_STATE,
                            generation);
        xSemaphoreGive(s_operation_lock);
        return ESP_ERR_INVALID_STATE;
    }

    session->grace_started_ms = 0;
    transition_locked(session,
                      RESOURCE_MANAGER_SESSION_ACTIVE,
                      now_ms(),
                      restore_reason);
    log_restore_transition(device_id,
                           RESOURCE_MANAGER_SESSION_RESTORING,
                           RESOURCE_MANAGER_SESSION_ACTIVE,
                           restore_reason,
                           session->generation);
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG,
             "RESOURCE_RESTORE device_id=%s reason=%s status=ok csi=warm command=active sensor=active",
             device_id,
             restore_reason);
    (void)network_worker_restore_peer_resources(device_id);
    xSemaphoreGive(s_operation_lock);
    return ESP_OK;
}

void resource_manager_tick(void)
{
    if (s_lock == NULL) {
        return;
    }

    const int64_t timestamp_ms = now_ms();
    const int64_t timestamp_us = now_us();
    const int64_t grace_ms = (int64_t)gateway_config_get()->link_lost_grace_ms;
    char active_ids[GATEWAY_CONFIG_MAX_CHILDREN][RESOURCE_MANAGER_DEVICE_ID_LEN] = {0};
    size_t active_count = 0U;
    char retry_ids[GATEWAY_CONFIG_MAX_CHILDREN][RESOURCE_MANAGER_DEVICE_ID_LEN] = {0};
    size_t retry_count = 0U;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        resource_session_t *session = &s_sessions[i];
        if (!session->used) {
            continue;
        }
        if (session->state == RESOURCE_MANAGER_SESSION_GRACE &&
            session->grace_started_ms > 0 &&
            timestamp_ms - session->grace_started_ms > grace_ms) {
            session->grace_started_ms = 0;
            transition_locked(session,
                              RESOURCE_MANAGER_SESSION_RELEASED,
                              timestamp_ms,
                              "link_lost_grace_expired");
        } else if (session->state == RESOURCE_MANAGER_SESSION_ACTIVE) {
            strlcpy(active_ids[active_count],
                    session->device_id,
                    sizeof(active_ids[active_count]));
            ++active_count;
        }
        if (session->release_pending && retry_count < GATEWAY_CONFIG_MAX_CHILDREN) {
            strlcpy(retry_ids[retry_count],
                    session->device_id,
                    sizeof(retry_ids[retry_count]));
            ++retry_count;
        }
    }
    xSemaphoreGive(s_lock);

    for (size_t i = 0; i < retry_count; ++i) {
        xSemaphoreTake(s_operation_lock, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        resource_session_t *session = find_locked(retry_ids[i]);
        const bool still_pending = session != NULL && session->release_pending &&
                                   !session_has_live_resources(session);
        const int64_t retry_cutoff_us = session != NULL ?
                                                session->restore_not_before_us :
                                                timestamp_us;
        xSemaphoreGive(s_lock);
        esp_err_t retry_ret = still_pending ?
                                  release_live_resources(retry_ids[i],
                                                         retry_cutoff_us,
                                                         "release_retry") :
                                  ESP_OK;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        session = find_locked(retry_ids[i]);
        if (session != NULL && still_pending) {
            session->release_pending = retry_ret != ESP_OK;
        }
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_operation_lock);
    }

    for (size_t i = 0; i < active_count; ++i) {
        child_registry_status_view_t view = {0};
        const bool registered = child_registry_get_status_view(active_ids[i], &view);
        bool recent_identity = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        resource_session_t *session = find_locked(active_ids[i]);
        if (session != NULL && session->last_identity_observed_us > 0) {
            const int64_t identity_age_us = timestamp_us - session->last_identity_observed_us;
            recent_identity = identity_age_us >= 0 &&
                              identity_age_us <=
                                  (int64_t)gateway_config_get()->heartbeat_timeout_ms * 1000;
        }
        xSemaphoreGive(s_lock);
        if ((!registered || view.status == CHILD_REGISTRY_STATUS_OFFLINE) &&
            !recent_identity) {
            (void)release_peer_to(active_ids[i],
                                  timestamp_ms,
                                  timestamp_us,
                                  "heartbeat_timeout",
                                  0U,
                                  RESOURCE_MANAGER_SESSION_RELEASED);
        }
    }
}

bool resource_manager_is_active(const char *device_id)
{
    if (s_lock == NULL) {
        return false;
    }
    bool active = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    resource_session_t *session = find_locked(device_id);
    active = session != NULL && session->state == RESOURCE_MANAGER_SESSION_ACTIVE;
    xSemaphoreGive(s_lock);
    return active;
}

bool resource_manager_is_live(const char *device_id)
{
    if (s_lock == NULL) {
        return false;
    }
    bool live = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    resource_session_t *session = find_locked(device_id);
    live = session != NULL && session_has_live_resources(session);
    xSemaphoreGive(s_lock);
    return live;
}

bool resource_manager_has_active_sessions(void)
{
    if (s_lock == NULL) {
        return false;
    }
    bool active = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        if (s_sessions[i].used &&
            s_sessions[i].state == RESOURCE_MANAGER_SESSION_ACTIVE) {
            active = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return active;
}

bool resource_manager_has_live_sessions(void)
{
    if (s_lock == NULL) {
        return false;
    }
    bool live = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN; ++i) {
        if (s_sessions[i].used && session_has_live_resources(&s_sessions[i])) {
            live = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return live;
}

size_t resource_manager_snapshot_active(
    char device_ids[][RESOURCE_MANAGER_DEVICE_ID_LEN],
    size_t capacity)
{
    if (device_ids == NULL || capacity == 0U || s_lock == NULL) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN && count < capacity; ++i) {
        if (s_sessions[i].used &&
            s_sessions[i].state == RESOURCE_MANAGER_SESSION_ACTIVE) {
            strlcpy(device_ids[count],
                    s_sessions[i].device_id,
                    RESOURCE_MANAGER_DEVICE_ID_LEN);
            ++count;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

size_t resource_manager_snapshot_live(
    char device_ids[][RESOURCE_MANAGER_DEVICE_ID_LEN],
    size_t capacity)
{
    if (device_ids == NULL || capacity == 0U || s_lock == NULL) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < GATEWAY_CONFIG_MAX_CHILDREN && count < capacity; ++i) {
        if (s_sessions[i].used && session_has_live_resources(&s_sessions[i])) {
            strlcpy(device_ids[count],
                    s_sessions[i].device_id,
                    RESOURCE_MANAGER_DEVICE_ID_LEN);
            ++count;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

static esp_err_t get_session_with_timeout(const char *device_id,
                                          resource_manager_session_view_t *out_view,
                                          TickType_t lock_timeout_ticks,
                                          bool *out_found)
{
    if (out_view == NULL || out_found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_found = false;
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out_view, 0, sizeof(*out_view));
    if (xSemaphoreTake(s_lock, lock_timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    resource_session_t *session = find_locked(device_id);
    if (session != NULL) {
        strlcpy(out_view->device_id, session->device_id, sizeof(out_view->device_id));
        out_view->state = session->state;
        out_view->state_since_ms = session->state_since_ms;
        out_view->grace_started_ms = session->grace_started_ms;
        out_view->generation = session->generation;
        *out_found = true;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool resource_manager_get_session(const char *device_id,
                                  resource_manager_session_view_t *out_view)
{
    bool found = false;
    return get_session_with_timeout(device_id, out_view, portMAX_DELAY, &found) == ESP_OK && found;
}

esp_err_t resource_manager_get_session_timed(const char *device_id,
                                             resource_manager_session_view_t *out_view,
                                             uint32_t lock_timeout_ms,
                                             bool *out_found)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(lock_timeout_ms);
    if (lock_timeout_ms > 0U && timeout_ticks == 0U) {
        timeout_ticks = 1U;
    }
    return get_session_with_timeout(device_id, out_view, timeout_ticks, out_found);
}

void resource_manager_log_session_diagnostic(const char *device_id,
                                             const char *link_id,
                                             const char *action,
                                             const char *reason)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return;
    }

    const int64_t timestamp_ms = now_ms();
    resource_manager_session_view_t session = {0};
    const bool found = resource_manager_get_session(device_id, &session);
    child_registry_status_view_t view = {0};
    (void)child_registry_get_status_view(device_id, &view);

    const int64_t last_seen_ms = view.last_seen_ms;
    int64_t age_ms = -1;
    if (last_seen_ms > 0) {
        age_ms = timestamp_ms >= last_seen_ms ? timestamp_ms - last_seen_ms : 0;
    }

    int64_t state_age_ms = -1;
    if (found && session.state_since_ms > 0) {
        state_age_ms = timestamp_ms >= session.state_since_ms ?
                           timestamp_ms - session.state_since_ms : 0;
    }

    ESP_LOGI(TAG,
             "SESSION_DIAG action=%s reason=%s device_id=%s link_id=%s session_state=%s last_seen_ms=%lld age_ms=%lld resource_generation=%lu state_age_ms=%lld",
             action != NULL ? action : "unknown",
             reason != NULL ? reason : "unknown",
             device_id,
             link_id != NULL && link_id[0] != '\0' ? link_id : link_id_for_device_id(device_id),
             found ? resource_manager_session_state_name(session.state) : "UNKNOWN",
             (long long)last_seen_ms,
             (long long)age_ms,
             (unsigned long)(found ? session.generation : 0U),
             (long long)state_age_ms);
}
