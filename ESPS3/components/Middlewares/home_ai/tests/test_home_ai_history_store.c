#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_history_store.h"

#define TEST_EVENTS_PATH "/tmp/home_ai_events.bin"
#define TEST_META_PATH "/tmp/home_ai_meta.bin"

static home_ai_history_event_t event_for(unsigned int index,
                                         uint8_t priority,
                                         uint64_t occurred_at_ms)
{
    home_ai_history_event_t event = {0};
    event.valid = true;
    event.priority = priority;
    event.occurred_at_ms = occurred_at_ms;
    snprintf(event.event_id, sizeof(event.event_id), "event_%u", index);
    strcpy(event.room_id, "bedroom_01");
    strcpy(event.event_type, "decision");
    snprintf(event.payload, sizeof(event.payload), "{\"index\":%u}", index);
    event.payload_len = (uint16_t)strlen(event.payload);
    return event;
}

static void reset_store_files(void)
{
    (void)remove(TEST_EVENTS_PATH);
    (void)remove(TEST_META_PATH);
}

static void persist_events(unsigned int first_index,
                           size_t count,
                           uint8_t priority,
                           uint64_t occurred_at_ms)
{
    size_t pending = 0U;
    for (size_t index = 0U; index < count; ++index) {
        home_ai_history_event_t event = event_for(first_index + (unsigned int)index,
                                                   priority,
                                                   occurred_at_ms);
        assert(home_ai_history_enqueue(&event) == ESP_OK);
        ++pending;
        if (pending == HOME_AI_HISTORY_PENDING_CAPACITY || index + 1U == count) {
            assert(home_ai_history_flush(pending) == pending);
            pending = 0U;
        }
    }
}

static size_t mark_all_uploaded(void)
{
    size_t marked = 0U;
    home_ai_history_event_t event = {0};
    while (home_ai_history_peek_unuploaded(&event) == ESP_OK) {
        assert(home_ai_history_mark_uploaded(event.sequence) == ESP_OK);
        ++marked;
        assert(marked <= HOME_AI_HISTORY_CAPACITY);
    }
    return marked;
}

static void test_basic_and_pending_priority(void)
{
    reset_store_files();
    assert(home_ai_history_store_init());

    home_ai_history_event_t first = event_for(1U, 100U, 1001U);
    home_ai_history_event_t second = event_for(2U, 200U, 1002U);
    assert(home_ai_history_enqueue(&first) == ESP_OK);
    assert(home_ai_history_enqueue(&second) == ESP_OK);
    assert(home_ai_history_flush(1U) == 1U);
    assert(home_ai_history_flush(8U) == 1U);

    home_ai_history_event_t replay = {0};
    assert(home_ai_history_peek_unuploaded(&replay) == ESP_OK);
    assert(strcmp(replay.event_id, "event_2") == 0);
    assert(home_ai_history_mark_uploaded(replay.sequence) == ESP_OK);
    assert(home_ai_history_peek_unuploaded(&replay) == ESP_OK);
    assert(strcmp(replay.event_id, "event_1") == 0);

    for (unsigned int index = 0U; index < HOME_AI_HISTORY_PENDING_CAPACITY; ++index) {
        home_ai_history_event_t event = event_for(100U + index, 10U, 2000U + index);
        assert(home_ai_history_enqueue(&event) == ESP_OK);
    }
    home_ai_history_event_t urgent = event_for(999U, 250U, 3000U);
    assert(home_ai_history_enqueue(&urgent) == ESP_OK);
    home_ai_history_stats_t stats = home_ai_history_get_stats();
    assert(stats.dropped_unpersisted == 1U);
    assert(stats.pending_ram_count == HOME_AI_HISTORY_PENDING_CAPACITY);
    assert(home_ai_history_flush(HOME_AI_HISTORY_PENDING_CAPACITY) ==
           HOME_AI_HISTORY_PENDING_CAPACITY);
    assert(home_ai_history_pending_count() >= HOME_AI_HISTORY_PENDING_CAPACITY + 1U);
}

static void test_restart_rebuild(void)
{
    assert(home_ai_history_store_init());
    home_ai_history_stats_t stats = home_ai_history_get_stats();
    assert(stats.persisted_count == HOME_AI_HISTORY_PENDING_CAPACITY + 2U);
    assert(stats.unuploaded_count == HOME_AI_HISTORY_PENDING_CAPACITY + 1U);
    home_ai_history_event_t replay = {0};
    assert(home_ai_history_peek_unuploaded(&replay) == ESP_OK);
    assert(strcmp(replay.event_id, "event_1") == 0);
}

static void test_best_effort_prune(void)
{
    const uint64_t occurred_at_ms = 1000U;
    reset_store_files();
    assert(home_ai_history_store_init());
    persist_events(10000U,
                   HOME_AI_HISTORY_CAPACITY,
                   20U,
                   occurred_at_ms);
    home_ai_history_stats_t stats = home_ai_history_get_stats();
    assert(stats.capacity_percent == 100U);
    assert(stats.capacity_warning);
    assert(mark_all_uploaded() == HOME_AI_HISTORY_CAPACITY);
    assert(home_ai_history_prune(occurred_at_ms +
                                 HOME_AI_HISTORY_BEST_EFFORT_RETENTION_MS) ==
           HOME_AI_HISTORY_CAPACITY);
    stats = home_ai_history_get_stats();
    assert(stats.persisted_count == 0U);
    assert(stats.capacity_percent == 0U);
    assert(!stats.capacity_warning);
    assert(stats.retention_evictions == HOME_AI_HISTORY_CAPACITY);
}

static void test_guaranteed_retention_rejects_overwrite(void)
{
    const uint64_t recent_time_ms = 10000000U;
    reset_store_files();
    assert(home_ai_history_store_init());
    persist_events(20000U,
                   HOME_AI_HISTORY_CAPACITY,
                   10U,
                   recent_time_ms);

    home_ai_history_event_t emergency = event_for(29999U, 255U, recent_time_ms + 1U);
    assert(home_ai_history_enqueue(&emergency) == ESP_OK);
    assert(home_ai_history_flush(1U) == 0U);
    home_ai_history_stats_t stats = home_ai_history_get_stats();
    assert(stats.protected_rejections == 1U);
    assert(stats.dropped_overwrite == 0U);
    assert(stats.persisted_count == HOME_AI_HISTORY_CAPACITY);
    assert(stats.unuploaded_count == HOME_AI_HISTORY_CAPACITY);
}

static void test_priority_replacement_after_guarantee(void)
{
    const uint64_t old_time_ms = 5000U;
    reset_store_files();
    assert(home_ai_history_store_init());
    persist_events(30000U,
                   HOME_AI_HISTORY_CAPACITY,
                   10U,
                   old_time_ms);

    home_ai_history_event_t emergency = event_for(
        39999U,
        250U,
        old_time_ms + HOME_AI_HISTORY_GUARANTEED_RETENTION_MS);
    assert(home_ai_history_enqueue(&emergency) == ESP_OK);
    assert(home_ai_history_flush(1U) == 1U);
    home_ai_history_stats_t stats = home_ai_history_get_stats();
    assert(stats.dropped_overwrite == 1U);
    assert(stats.persisted_count == HOME_AI_HISTORY_CAPACITY);
    assert(stats.unuploaded_count == HOME_AI_HISTORY_CAPACITY);

    bool found_emergency = false;
    home_ai_history_event_t replay = {0};
    for (size_t index = 0U; index < HOME_AI_HISTORY_CAPACITY; ++index) {
        assert(home_ai_history_peek_unuploaded(&replay) == ESP_OK);
        if (strcmp(replay.event_id, "event_39999") == 0) found_emergency = true;
        assert(home_ai_history_mark_uploaded(replay.sequence) == ESP_OK);
    }
    assert(found_emergency);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "basic") == 0) {
        test_basic_and_pending_priority();
    } else if (strcmp(argv[1], "restart") == 0) {
        test_restart_rebuild();
    } else if (strcmp(argv[1], "prune") == 0) {
        test_best_effort_prune();
    } else if (strcmp(argv[1], "protected") == 0) {
        test_guaranteed_retention_rejects_overwrite();
    } else if (strcmp(argv[1], "priority") == 0) {
        test_priority_replacement_after_guarantee();
    } else {
        assert(!"unknown test mode");
    }
    puts("home ai history store host tests: PASS");
    return 0;
}
