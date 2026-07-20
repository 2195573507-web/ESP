#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_config_store.h"

static void fill_configs(home_ai_room_state_config_t *configs)
{
    memset(configs, 0, HOME_AI_ROOM_STATE_COUNT * sizeof(*configs));
    const char *rooms[HOME_AI_ROOM_STATE_COUNT] = {"living_room", "bedroom_01", "bedroom_02"};
    const char *terminals[HOME_AI_ROOM_STATE_COUNT] = {
        "", "sensair_shuttle_01", "sensair_shuttle_02"
    };
    for (size_t index = 0U; index < HOME_AI_ROOM_STATE_COUNT; ++index) {
        configs[index].source = (radar_source_id_t)index;
        strcpy(configs[index].room_id, rooms[index]);
        strcpy(configs[index].voice_terminal_device_id, terminals[index]);
        configs[index].occupied_confirm_ms = 1500U;
        configs[index].vacant_confirm_ms = 60000U;
        configs[index].multiple_confirm_ms = 3000U;
        configs[index].single_confirm_ms = 10000U;
        configs[index].quiet_start_minute = 1380U;
        configs[index].quiet_end_minute = 420U;
    }
}

int main(void)
{
    (void)remove(HOME_AI_CONFIG_STORE_PATH);
    (void)remove(HOME_AI_CONFIG_STORE_TMP_PATH);
    home_ai_room_state_config_t expected[HOME_AI_ROOM_STATE_COUNT];
    home_ai_room_state_config_t loaded[HOME_AI_ROOM_STATE_COUNT];
    fill_configs(expected);
    assert(home_ai_config_store_save(expected, HOME_AI_ROOM_STATE_COUNT));
    assert(home_ai_config_store_load(loaded, HOME_AI_ROOM_STATE_COUNT));
    assert(memcmp(expected, loaded, sizeof(expected)) == 0);

    strcpy(expected[RADAR_SOURCE_C51].room_id, "sleep_room");
    assert(home_ai_config_store_save(expected, HOME_AI_ROOM_STATE_COUNT));
    assert(home_ai_config_store_load(loaded, HOME_AI_ROOM_STATE_COUNT));
    assert(strcmp(loaded[RADAR_SOURCE_C51].room_id, "sleep_room") == 0);

    FILE *file = fopen(HOME_AI_CONFIG_STORE_PATH, "r+b");
    assert(file != NULL);
    assert(fseek(file, 16L, SEEK_SET) == 0);
    const int value = fgetc(file);
    assert(value != EOF);
    assert(fseek(file, 16L, SEEK_SET) == 0);
    assert(fputc(value ^ 0x5a, file) != EOF);
    assert(fclose(file) == 0);
    assert(!home_ai_config_store_load(loaded, HOME_AI_ROOM_STATE_COUNT));

    file = fopen(HOME_AI_CONFIG_STORE_PATH, "wb");
    assert(file != NULL);
    assert(fwrite("short", 5U, 1U, file) == 1U);
    assert(fclose(file) == 0);
    assert(!home_ai_config_store_load(loaded, HOME_AI_ROOM_STATE_COUNT));
    assert(!home_ai_config_store_save(expected, HOME_AI_ROOM_STATE_COUNT - 1U));

    (void)remove(HOME_AI_CONFIG_STORE_PATH);
    (void)remove(HOME_AI_CONFIG_STORE_TMP_PATH);
    puts("home ai config store host tests: PASS");
    return 0;
}
