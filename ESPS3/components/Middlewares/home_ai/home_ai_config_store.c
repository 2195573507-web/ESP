#include "home_ai_config_store.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HOME_AI_CONFIG_STORE_MAGIC 0x48414943U
#define HOME_AI_CONFIG_STORE_VERSION 1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t room_count;
    home_ai_room_state_config_t rooms[HOME_AI_ROOM_STATE_COUNT];
    uint32_t crc32;
} home_ai_config_snapshot_t;

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffU;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

bool home_ai_config_store_save(const home_ai_room_state_config_t *configs, size_t count)
{
    if (configs == NULL || count != HOME_AI_ROOM_STATE_COUNT) return false;
    home_ai_config_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = HOME_AI_CONFIG_STORE_MAGIC;
    snapshot.version = HOME_AI_CONFIG_STORE_VERSION;
    snapshot.room_count = (uint16_t)count;
    memcpy(snapshot.rooms, configs, sizeof(snapshot.rooms));
    snapshot.crc32 = crc32((const uint8_t *)&snapshot, offsetof(home_ai_config_snapshot_t, crc32));

    FILE *file = fopen(HOME_AI_CONFIG_STORE_TMP_PATH, "wb");
    if (file == NULL) return false;
    const bool written = fwrite(&snapshot, sizeof(snapshot), 1U, file) == 1U && fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !closed) {
        (void)remove(HOME_AI_CONFIG_STORE_TMP_PATH);
        return false;
    }
    (void)remove(HOME_AI_CONFIG_STORE_PATH);
    if (rename(HOME_AI_CONFIG_STORE_TMP_PATH, HOME_AI_CONFIG_STORE_PATH) == 0) return true;
    (void)remove(HOME_AI_CONFIG_STORE_TMP_PATH);
    return false;
}

bool home_ai_config_store_load(home_ai_room_state_config_t *out_configs, size_t count)
{
    if (out_configs == NULL || count != HOME_AI_ROOM_STATE_COUNT) return false;
    memset(out_configs, 0, count * sizeof(*out_configs));
    FILE *file = fopen(HOME_AI_CONFIG_STORE_PATH, "rb");
    if (file == NULL) return false;
    home_ai_config_snapshot_t snapshot;
    const bool read_ok = fread(&snapshot, sizeof(snapshot), 1U, file) == 1U && fgetc(file) == EOF;
    (void)fclose(file);
    if (!read_ok || snapshot.magic != HOME_AI_CONFIG_STORE_MAGIC ||
        snapshot.version != HOME_AI_CONFIG_STORE_VERSION || snapshot.room_count != count ||
        snapshot.crc32 != crc32((const uint8_t *)&snapshot, offsetof(home_ai_config_snapshot_t, crc32))) {
        return false;
    }
    memcpy(out_configs, snapshot.rooms, sizeof(snapshot.rooms));
    return true;
}
