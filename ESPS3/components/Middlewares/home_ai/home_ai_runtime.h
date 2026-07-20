#ifndef HOME_AI_RUNTIME_H
#define HOME_AI_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime owns no task. The existing scheduler and network worker call these entry points. */
bool home_ai_runtime_init(void);
void home_ai_runtime_tick(uint64_t now_ms);
esp_err_t home_ai_runtime_sync_rules_now(void);
esp_err_t home_ai_runtime_check_rule_notification(void);
bool home_ai_runtime_rollback_rules(void);
uint32_t home_ai_runtime_active_rule_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_RUNTIME_H */
