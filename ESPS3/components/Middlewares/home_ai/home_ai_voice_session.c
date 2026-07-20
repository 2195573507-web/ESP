#include "home_ai_voice_session.h"

#include <stdio.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifndef HOME_AI_VOICE_SESSION_LEASE_MS
#define HOME_AI_VOICE_SESSION_LEASE_MS 30000U
#endif

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static home_ai_voice_session_lease_t s_lease;
static uint32_t s_generation;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void clear_lease_locked(home_ai_voice_session_state_t final_state)
{
    s_lease.voice_session_id[0] = '\0';
    s_lease.owner_device_id[0] = '\0';
    s_lease.lease_expires_at_ms = 0;
    s_lease.state = final_state;
}

static void expire_locked(void)
{
    if (s_lease.owner_device_id[0] != '\0' && now_ms() >= s_lease.lease_expires_at_ms) {
        clear_lease_locked(HOME_AI_VOICE_SESSION_IDLE);
    }
}

static bool matches_locked(const char *device_id,
                           const char *voice_session_id,
                           uint32_t generation)
{
    return device_id != NULL && voice_session_id != NULL && device_id[0] != '\0' &&
           voice_session_id[0] != '\0' && s_lease.owner_device_id[0] != '\0' &&
           generation == s_lease.generation &&
           strcmp(device_id, s_lease.owner_device_id) == 0 &&
           strcmp(voice_session_id, s_lease.voice_session_id) == 0;
}

static bool transition_allowed(home_ai_voice_session_state_t current,
                               home_ai_voice_session_state_t next)
{
    if (current == next) return true;
    switch (current) {
    case HOME_AI_VOICE_SESSION_LOCKED:
        return next == HOME_AI_VOICE_SESSION_RECORDING ||
               next == HOME_AI_VOICE_SESSION_ENDING;
    case HOME_AI_VOICE_SESSION_RECORDING:
        return next == HOME_AI_VOICE_SESSION_WAITING_SERVER ||
               next == HOME_AI_VOICE_SESSION_ENDING;
    case HOME_AI_VOICE_SESSION_WAITING_SERVER:
        return next == HOME_AI_VOICE_SESSION_PLAYING ||
               next == HOME_AI_VOICE_SESSION_ENDING;
    case HOME_AI_VOICE_SESSION_PLAYING:
        return next == HOME_AI_VOICE_SESSION_ENDING;
    case HOME_AI_VOICE_SESSION_ENDING:
        return next == HOME_AI_VOICE_SESSION_ENDING;
    default:
        return false;
    }
}

esp_err_t home_ai_voice_session_manager_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_lease, 0, sizeof(s_lease));
    s_lease.state = HOME_AI_VOICE_SESSION_IDLE;
    s_generation = 0U;
    return ESP_OK;
}

esp_err_t home_ai_voice_session_acquire(const char *device_id,
                                        home_ai_voice_session_lease_t *out_lease)
{
    if (device_id == NULL || device_id[0] == '\0' || out_lease == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    expire_locked();
    if (s_lease.owner_device_id[0] != '\0') {
        *out_lease = s_lease;
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_generation++;
    if (s_generation == 0U) {
        s_generation = 1U;
    }
    const uint32_t entropy = esp_random();
    snprintf(s_lease.voice_session_id,
             sizeof(s_lease.voice_session_id),
             "vs_%08lx_%08lx",
             (unsigned long)s_generation,
             (unsigned long)entropy);
    strlcpy(s_lease.owner_device_id, device_id, sizeof(s_lease.owner_device_id));
    s_lease.generation = s_generation;
    s_lease.lease_expires_at_ms = now_ms() + HOME_AI_VOICE_SESSION_LEASE_MS;
    s_lease.state = HOME_AI_VOICE_SESSION_LOCKED;
    *out_lease = s_lease;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t home_ai_voice_session_renew(const char *device_id,
                                      const char *voice_session_id,
                                      uint32_t generation,
                                      home_ai_voice_session_lease_t *out_lease)
{
    if (out_lease == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    expire_locked();
    if (!matches_locked(device_id, voice_session_id, generation)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lease.lease_expires_at_ms = now_ms() + HOME_AI_VOICE_SESSION_LEASE_MS;
    *out_lease = s_lease;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t home_ai_voice_session_transition(const char *device_id,
                                           const char *voice_session_id,
                                           uint32_t generation,
                                           home_ai_voice_session_state_t next_state,
                                           home_ai_voice_session_lease_t *out_lease)
{
    if (out_lease == NULL || s_lock == NULL ||
        next_state < HOME_AI_VOICE_SESSION_LOCKED ||
        next_state > HOME_AI_VOICE_SESSION_ENDING) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    expire_locked();
    if (!matches_locked(device_id, voice_session_id, generation) ||
        !transition_allowed(s_lease.state, next_state)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_lease.state = next_state;
    s_lease.lease_expires_at_ms = now_ms() + HOME_AI_VOICE_SESSION_LEASE_MS;
    *out_lease = s_lease;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t home_ai_voice_session_release(const char *device_id,
                                        const char *voice_session_id,
                                        uint32_t generation)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    expire_locked();
    if (!matches_locked(device_id, voice_session_id, generation)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    clear_lease_locked(HOME_AI_VOICE_SESSION_IDLE);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool home_ai_voice_session_release_owner(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0' || s_lock == NULL ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
        return false;
    }
    expire_locked();
    const bool owner_matches = s_lease.owner_device_id[0] != '\0' &&
                               strcmp(device_id, s_lease.owner_device_id) == 0;
    if (owner_matches) {
        clear_lease_locked(HOME_AI_VOICE_SESSION_IDLE);
    }
    xSemaphoreGive(s_lock);
    return owner_matches;
}

bool home_ai_voice_session_validate(const char *device_id,
                                    const char *voice_session_id,
                                    uint32_t generation)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) {
        return false;
    }
    expire_locked();
    const bool valid = matches_locked(device_id, voice_session_id, generation);
    xSemaphoreGive(s_lock);
    return valid;
}

bool home_ai_voice_session_get(home_ai_voice_session_lease_t *out_lease)
{
    if (out_lease == NULL || s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) {
        return false;
    }
    expire_locked();
    *out_lease = s_lease;
    const bool active = s_lease.owner_device_id[0] != '\0';
    xSemaphoreGive(s_lock);
    return active;
}

bool home_ai_voice_session_preempt_for_emergency(home_ai_voice_session_lease_t *out_preempted)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
        return false;
    }
    expire_locked();
    const bool active = s_lease.owner_device_id[0] != '\0';
    if (active && out_preempted != NULL) {
        *out_preempted = s_lease;
    }
    if (active) {
        s_generation++;
        if (s_generation == 0U) {
            s_generation = 1U;
        }
        clear_lease_locked(HOME_AI_VOICE_SESSION_PREEMPTED);
    }
    xSemaphoreGive(s_lock);
    return active;
}

const char *home_ai_voice_session_state_name(home_ai_voice_session_state_t state)
{
    switch (state) {
    case HOME_AI_VOICE_SESSION_IDLE:
        return "IDLE";
    case HOME_AI_VOICE_SESSION_LOCKED:
        return "LOCKED";
    case HOME_AI_VOICE_SESSION_RECORDING:
        return "RECORDING";
    case HOME_AI_VOICE_SESSION_WAITING_SERVER:
        return "WAITING_SERVER";
    case HOME_AI_VOICE_SESSION_PLAYING:
        return "PLAYING";
    case HOME_AI_VOICE_SESSION_ENDING:
        return "ENDING";
    case HOME_AI_VOICE_SESSION_PREEMPTED:
        return "PREEMPTED";
    default:
        return "UNKNOWN";
    }
}

bool home_ai_voice_session_state_from_name(const char *name,
                                           home_ai_voice_session_state_t *out_state)
{
    if (name == NULL || out_state == NULL) return false;
    if (strcmp(name, "LOCKED") == 0) *out_state = HOME_AI_VOICE_SESSION_LOCKED;
    else if (strcmp(name, "RECORDING") == 0) *out_state = HOME_AI_VOICE_SESSION_RECORDING;
    else if (strcmp(name, "WAITING_SERVER") == 0) *out_state = HOME_AI_VOICE_SESSION_WAITING_SERVER;
    else if (strcmp(name, "PLAYING") == 0) *out_state = HOME_AI_VOICE_SESSION_PLAYING;
    else if (strcmp(name, "ENDING") == 0) *out_state = HOME_AI_VOICE_SESSION_ENDING;
    else return false;
    return true;
}
