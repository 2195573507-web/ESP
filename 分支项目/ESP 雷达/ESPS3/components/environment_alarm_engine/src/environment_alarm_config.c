#include "environment_alarm_engine_internal.h"

const alarm_engine_config_t g_alarm_config = {
    .high_temperature_trigger=35, .high_temperature_recovery=33, .low_temperature_trigger=10, .low_temperature_recovery=12,
    .high_humidity_trigger=75, .high_humidity_recovery=70, .low_humidity_trigger=25, .low_humidity_recovery=30,
    .fast_temperature_trigger=3, .fast_temperature_recovery=1, .fast_humidity_trigger=15, .fast_humidity_recovery=8,
    .aq_warning_trigger=49, .aq_warning_recovery=55, .aq_critical_trigger=29, .aq_critical_recovery=35,
    .aq_deteriorating_delta=20, .aq_deteriorating_recovery_delta=10, .aq_deteriorating_current_max=70, .aq_deteriorating_gas_ratio_max=.85f,
    .pollution_trigger=.70f, .pollution_critical=.55f, .pollution_recovery=.80f, .stability_trigger=.55f, .stability_recovery=.70f,
    .critical_environment_high_temperature=40, .critical_environment_low_temperature=5, .critical_environment_high_humidity=85, .critical_environment_low_humidity=15,
    .trigger_60s_ms=60000, .trigger_30s_ms=30000, .recovery_120s_ms=120000, .recovery_10min_ms=600000,
    .combination_trigger_ms=60000, .combination_recovery_ms=300000, .max_inter_sample_gap_ms=45000,
    .temperature_window_ms=300000, .air_quality_window_ms=600000, .diagnostic_log_interval_ms=60000, .rule_version=ALARM_ENGINE_RULE_VERSION,
    .queue_capacity=ALARM_ENGINE_MAX_EVENTS, .minimum_samples_one=1, .minimum_samples_two=2, .minimum_samples_three=3,
    .allowed_devices={ALARM_DEVICE_C51,ALARM_DEVICE_C52}, .allowed_device_count=2,
};

bool alarm_config_is_valid(void) {
    const alarm_engine_config_t *c=&g_alarm_config;
    if (c->high_temperature_recovery>=c->high_temperature_trigger || c->low_temperature_recovery<=c->low_temperature_trigger ||
        c->high_humidity_recovery>=c->high_humidity_trigger || c->low_humidity_recovery<=c->low_humidity_trigger ||
        c->fast_temperature_recovery>=c->fast_temperature_trigger || c->fast_humidity_recovery>=c->fast_humidity_trigger ||
        c->aq_warning_recovery<=c->aq_warning_trigger || c->aq_critical_recovery<=c->aq_critical_trigger ||
        c->pollution_recovery<=c->pollution_trigger || c->stability_recovery<=c->stability_trigger ||
        c->trigger_60s_ms==0 || c->trigger_30s_ms==0 || c->recovery_120s_ms==0 || c->recovery_10min_ms==0 ||
        c->combination_trigger_ms==0 || c->combination_recovery_ms==0 || c->max_inter_sample_gap_ms==0 ||
        c->temperature_window_ms==0 || c->air_quality_window_ms==0 || c->queue_capacity<ALARM_ENGINE_MAX_EVENTS_PER_UPDATE ||
        c->queue_capacity>ALARM_ENGINE_MAX_EVENTS || c->allowed_device_count==0 || c->allowed_device_count>ALARM_ENGINE_MAX_DEVICES) return false;
    for (uint8_t i=0;i<c->allowed_device_count;i++) { if (c->allowed_devices[i]>=ALARM_DEVICE_INVALID) return false;
        for (uint8_t j=(uint8_t)(i+1);j<c->allowed_device_count;j++) if (c->allowed_devices[i]==c->allowed_devices[j]) return false; }
    return c->aq_warning_trigger>=0 && c->aq_warning_recovery<=100 && c->aq_critical_trigger>=0 && c->aq_critical_recovery<=100 &&
           c->pollution_critical>0 && c->pollution_recovery<=10 && c->stability_trigger>=0 && c->stability_recovery<=1;
}
