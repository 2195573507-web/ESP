#ifndef CJSON_H
#define CJSON_H

#include <stddef.h>

typedef struct cJSON {
    int unused;
} cJSON;

cJSON *cJSON_CreateObject(void);
cJSON *cJSON_AddObjectToObject(cJSON *object, const char *name);
cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *value);
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, int value);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double value);
cJSON *cJSON_AddNullToObject(cJSON *object, const char *name);
int cJSON_PrintPreallocated(cJSON *item, char *buffer, int length, int format);
void cJSON_Delete(cJSON *item);
void *cJSON_malloc(size_t size);
void cJSON_free(void *pointer);

#endif /* CJSON_H */
