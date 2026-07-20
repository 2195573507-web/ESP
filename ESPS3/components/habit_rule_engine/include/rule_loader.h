#ifndef RULE_LOADER_H
#define RULE_LOADER_H

#include "habit_rule_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

void rule_loader_load_defaults(habit_rule_engine_t *engine);
esp_err_t rule_loader_load_json(habit_rule_engine_t *engine, const char *json);
esp_err_t rule_loader_load_remote_rule(habit_rule_engine_t *engine);

#ifdef __cplusplus
}
#endif

#endif
