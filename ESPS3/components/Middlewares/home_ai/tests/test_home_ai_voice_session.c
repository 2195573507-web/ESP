#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "home_ai_voice_session.h"

static int64_t s_now_us;
static uint32_t s_entropy = 0x12340000U;
static StaticSemaphore_t *s_mutex_storage;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

uint32_t esp_random(void)
{
    return ++s_entropy;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    s_mutex_storage = storage;
    return storage;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned int wait_ticks)
{
    (void)semaphore;
    (void)wait_ticks;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return pdTRUE;
}

int main(void)
{
    home_ai_voice_session_lease_t c51 = {0};
    home_ai_voice_session_lease_t c52 = {0};
    home_ai_voice_session_lease_t preempted = {0};

    assert(home_ai_voice_session_manager_init() == ESP_OK);
    assert(s_mutex_storage != NULL);
    assert(home_ai_voice_session_acquire("sensair_shuttle_01", &c51) == ESP_OK);
    assert(c51.generation == 1U);
    assert(home_ai_voice_session_validate("sensair_shuttle_01",
                                          c51.voice_session_id,
                                          c51.generation));

    assert(home_ai_voice_session_acquire("sensair_shuttle_02", &c52) == ESP_ERR_INVALID_STATE);
    assert(c52.generation == c51.generation);
    assert(home_ai_voice_session_renew("sensair_shuttle_01",
                                       c51.voice_session_id,
                                       c51.generation,
                                       &c51) == ESP_OK);
    assert(c51.state == HOME_AI_VOICE_SESSION_LOCKED);
    assert(home_ai_voice_session_transition("sensair_shuttle_01",
                                            c51.voice_session_id,
                                            c51.generation,
                                            HOME_AI_VOICE_SESSION_RECORDING,
                                            &c51) == ESP_OK);
    assert(c51.state == HOME_AI_VOICE_SESSION_RECORDING);
    assert(home_ai_voice_session_transition("sensair_shuttle_01",
                                            c51.voice_session_id,
                                            c51.generation,
                                            HOME_AI_VOICE_SESSION_PLAYING,
                                            &c51) == ESP_ERR_INVALID_STATE);
    assert(home_ai_voice_session_transition("sensair_shuttle_01",
                                            c51.voice_session_id,
                                            c51.generation,
                                            HOME_AI_VOICE_SESSION_WAITING_SERVER,
                                            &c51) == ESP_OK);
    assert(home_ai_voice_session_transition("sensair_shuttle_01",
                                            c51.voice_session_id,
                                            c51.generation,
                                            HOME_AI_VOICE_SESSION_PLAYING,
                                            &c51) == ESP_OK);

    s_now_us = c51.lease_expires_at_ms * 1000LL + 1LL;
    assert(!home_ai_voice_session_validate("sensair_shuttle_01",
                                           c51.voice_session_id,
                                           c51.generation));
    assert(home_ai_voice_session_acquire("sensair_shuttle_02", &c52) == ESP_OK);
    assert(c52.generation == 2U);

    assert(home_ai_voice_session_preempt_for_emergency(&preempted));
    assert(preempted.generation == c52.generation);
    assert(!home_ai_voice_session_validate("sensair_shuttle_02",
                                           c52.voice_session_id,
                                           c52.generation));
    assert(home_ai_voice_session_acquire("sensair_shuttle_01", &c51) == ESP_OK);
    assert(c51.generation == 4U);
    assert(home_ai_voice_session_release("sensair_shuttle_01",
                                         c51.voice_session_id,
                                         c51.generation) == ESP_OK);

    assert(home_ai_voice_session_acquire("sensair_shuttle_02", &c52) == ESP_OK);
    assert(c52.generation == 5U);
    assert(!home_ai_voice_session_release_owner("sensair_shuttle_01"));
    assert(home_ai_voice_session_validate("sensair_shuttle_02",
                                          c52.voice_session_id,
                                          c52.generation));
    assert(home_ai_voice_session_release_owner("sensair_shuttle_02"));
    assert(!home_ai_voice_session_validate("sensair_shuttle_02",
                                           c52.voice_session_id,
                                           c52.generation));
    assert(home_ai_voice_session_acquire("sensair_shuttle_01", &c51) == ESP_OK);
    assert(c51.generation == 6U);

    puts("home ai voice session host tests: PASS");
    return 0;
}
