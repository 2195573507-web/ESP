#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

/**
 * @file resource_manager.h
 * @brief ESPS3 per-child live resource session lifecycle.
 *
 * Identity retention remains owned by child_registry. This component owns only
 * live command, sensor, and CSI resources for an allowlisted C5.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RESOURCE_MANAGER_DEVICE_ID_LEN 48U

typedef enum {
    RESOURCE_MANAGER_SESSION_ACTIVE = 0,
    RESOURCE_MANAGER_SESSION_GRACE,
    RESOURCE_MANAGER_SESSION_RELEASED,
    RESOURCE_MANAGER_SESSION_RESTORING,
} resource_manager_session_state_t;

typedef enum {
    RESOURCE_MANAGER_SIGNAL_REGISTER = 0,
    RESOURCE_MANAGER_SIGNAL_HEARTBEAT,
    RESOURCE_MANAGER_SIGNAL_STATUS,
    RESOURCE_MANAGER_SIGNAL_SENSOR,
    RESOURCE_MANAGER_SIGNAL_CSI,
} resource_manager_identity_signal_t;

typedef struct {
    char device_id[RESOURCE_MANAGER_DEVICE_ID_LEN];
    resource_manager_session_state_t state;
    int64_t state_since_ms;
    int64_t grace_started_ms;
    uint32_t generation;
} resource_manager_session_view_t;

/** @brief Initialize allowlisted sessions in RELEASED with all live resources off. */
esp_err_t resource_manager_init(void);

/** @brief Release one mapped C5 into GRACE at the AP disconnect timestamp. */
esp_err_t resource_manager_release_peer(const char *device_id,
                                        int64_t disconnected_at_ms,
                                        const char *reason);
/** @brief Microsecond-ordered release used by the live Wi-Fi event path. */
esp_err_t resource_manager_release_peer_at_us(const char *device_id,
                                              int64_t disconnected_at_us,
                                              const char *reason);
/** @brief Release exactly the matched child/session identity; rejects stale sessions. */
esp_err_t resource_manager_release_child_by_identity(const char *device_id,
                                                     uint32_t session_generation,
                                                     int64_t disconnected_at_us,
                                                     const char *reason);

/** @brief Explicit global release for SoftAP stop, gateway reset, or reset APIs only. */
void resource_manager_release_all(int64_t disconnected_at_ms, const char *reason);
/** @brief Microsecond-ordered explicit global release; never use for unknown stations. */
void resource_manager_release_all_at_us(int64_t disconnected_at_us, const char *reason);

/**
 * @brief Confirm identity with validated post-disconnect data and restore resources.
 *
 * AP_STACONNECTED is not a valid confirmation signal. An ingress received before
 * the current disconnect is rejected even if processed later.
 */
esp_err_t resource_manager_confirm_peer(const char *device_id,
                                        const char *peer_ip,
                                        resource_manager_identity_signal_t signal,
                                        int64_t observed_at_ms);
/** @brief Microsecond-ordered identity confirmation used by live ingress. */
esp_err_t resource_manager_confirm_peer_at_us(const char *device_id,
                                              const char *peer_ip,
                                              resource_manager_identity_signal_t signal,
                                              int64_t observed_at_us);

/**
 * @brief AP_STACONNECTED can prepare a RELEASED/GRACE session for restore.
 *
 * This never marks the peer ACTIVE and never restores resources by itself. A
 * later validated register/heartbeat/status/sensor/CSI confirmation must finish
 * the transition. Peer IP is an optional transport mapping and is not identity.
 */
esp_err_t resource_manager_prepare_reconnect_at_us(const char *device_id,
                                                   int64_t observed_at_us,
                                                   const char *reason);

/** @brief Compatibility no-op once ingress restoration has already committed ACTIVE. */
esp_err_t resource_manager_complete_restore(const char *device_id, const char *reason);

/** @brief Advance GRACE expiry and reconcile the existing 30-second heartbeat timeout. */
void resource_manager_tick(void);

/** @brief Return true only when all live resources for this peer are ACTIVE. */
bool resource_manager_is_active(const char *device_id);

/** @brief Return true only after restored resources are usable. */
bool resource_manager_is_live(const char *device_id);

/** @brief Return true when at least one child session is ACTIVE. */
bool resource_manager_has_active_sessions(void);

/** @brief Return true when at least one peer has usable live resources. */
bool resource_manager_has_live_sessions(void);

/** @brief Snapshot ACTIVE device IDs; returns copied count. */
size_t resource_manager_snapshot_active(
    char device_ids[][RESOURCE_MANAGER_DEVICE_ID_LEN],
    size_t capacity);

/** @brief Snapshot peers with usable live resources; returns copied count. */
size_t resource_manager_snapshot_live(
    char device_ids[][RESOURCE_MANAGER_DEVICE_ID_LEN],
    size_t capacity);

/** @brief Read one session view without exposing internal storage. */
bool resource_manager_get_session(const char *device_id,
                                  resource_manager_session_view_t *out_view);

/**
 * @brief Read one session view while bounding the manager mutex wait.
 *
 * `ESP_OK` means the lock was acquired; `out_found` then distinguishes a
 * missing session from a populated view. HTTP ingress uses this to fail
 * admission instead of waiting indefinitely behind lifecycle work.
 */
esp_err_t resource_manager_get_session_timed(const char *device_id,
                                             resource_manager_session_view_t *out_view,
                                             uint32_t lock_timeout_ms,
                                             bool *out_found);

/** @brief Emit a consistent session diagnostic log for lifecycle boundaries. */
void resource_manager_log_session_diagnostic(const char *device_id,
                                             const char *link_id,
                                             const char *action,
                                             const char *reason);

const char *resource_manager_session_state_name(resource_manager_session_state_t state);
const char *resource_manager_identity_signal_name(resource_manager_identity_signal_t signal);

#ifdef __cplusplus
}
#endif

#endif /* RESOURCE_MANAGER_H */
