#ifndef HOME_AI_CONFIG_STORE_H
#define HOME_AI_CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "home_ai_room_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HOME_AI_CONFIG_STORE_HOST_TEST
#define HOME_AI_CONFIG_STORE_PATH "/tmp/home_ai_room_config_snapshot.bin"
#define HOME_AI_CONFIG_STORE_TMP_PATH "/tmp/home_ai_room_config_snapshot.tmp"
#else
#define HOME_AI_CONFIG_STORE_PATH "/home_ai/room_config_snapshot.bin"
#define HOME_AI_CONFIG_STORE_TMP_PATH "/home_ai/room_config_snapshot.tmp"
#endif

bool home_ai_config_store_save(const home_ai_room_state_config_t *configs, size_t count);
bool home_ai_config_store_load(home_ai_room_state_config_t *out_configs, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_CONFIG_STORE_H */
