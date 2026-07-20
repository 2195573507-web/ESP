#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "network_worker.h"

cJSON *cJSON_CreateObject(void)
{
    return calloc(1U, sizeof(cJSON));
}

cJSON *cJSON_AddObjectToObject(cJSON *object, const char *name)
{
    (void)name;
    return object != NULL ? cJSON_CreateObject() : NULL;
}

cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *value)
{
    (void)name;
    (void)value;
    return object;
}

cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, int value)
{
    (void)name;
    (void)value;
    return object;
}

cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double value)
{
    (void)name;
    (void)value;
    return object;
}

cJSON *cJSON_AddNullToObject(cJSON *object, const char *name)
{
    (void)name;
    return object;
}

int cJSON_PrintPreallocated(cJSON *item, char *buffer, int length, int format)
{
    (void)item;
    (void)format;
    if (buffer == NULL || length < 3) {
        return 0;
    }
    memcpy(buffer, "{}", 3U);
    return 1;
}

void cJSON_Delete(cJSON *item)
{
    free(item);
}

void *cJSON_malloc(size_t size)
{
    return malloc(size);
}

void cJSON_free(void *pointer)
{
    free(pointer);
}

bool network_worker_is_server_ready(void)
{
    return false;
}

network_worker_link_state_t network_worker_get_link_state(void)
{
    return NETWORK_WORKER_LINK_DOWN;
}

esp_err_t network_worker_submit_environment_alarm_json(
    char *json_body,
    uint64_t event_seq,
    network_worker_environment_alarm_completion_fn completion,
    void *completion_context,
    const char *source)
{
    (void)json_body;
    (void)event_seq;
    (void)completion;
    (void)completion_context;
    (void)source;
    return ESP_ERR_INVALID_STATE;
}
