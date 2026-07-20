#ifndef GATEWAY_CONFIG_H
#define GATEWAY_CONFIG_H

typedef struct {
    const char *gateway_id;
} gateway_runtime_config_t;

const gateway_runtime_config_t *gateway_config_get(void);

#endif /* GATEWAY_CONFIG_H */
