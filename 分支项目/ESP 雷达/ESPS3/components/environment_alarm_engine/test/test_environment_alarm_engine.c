#include "unity.h"

#include "environment_alarm_engine.h"

static uint64_t s_now;
static uint64_t clock_ms(void *unused) { (void)unused; return s_now; }
static alarm_environment_sample_t sample(uint64_t seq) {
    return (alarm_environment_sample_t){.struct_version=ALARM_ENVIRONMENT_SAMPLE_VERSION,.device_id=ALARM_DEVICE_C51,.ingest_seq=seq,.temperature=20,.humidity=50,.pressure=1000,.gas_resistance=10000,.air_quality_score=80,.air_quality_level=ALARM_AIR_QUALITY_GOOD,.gas_ratio=1,.stability_score=.9f,.sensor_state=ALARM_SENSOR_READY};
}
static void init(void) { s_now=0; TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_init(&(alarm_engine_options_t){.monotonic_clock_ms=clock_ms})); }

TEST_CASE("temperature requires duration and three samples", "[environment_alarm]") {
    init(); alarm_environment_sample_t s=sample(1); s.temperature=36; alarm_event_t e[2];
    TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL)); s_now=30000;s.ingest_seq=2;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));
    TEST_ASSERT_EQUAL_UINT32(0,alarm_engine_peek_events(e,2)); s_now=60000;s.ingest_seq=3;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));
    TEST_ASSERT_EQUAL_UINT32(1,alarm_engine_peek_events(e,2)); TEST_ASSERT_EQUAL(ALARM_TYPE_HIGH_TEMPERATURE,e[0].alarm_type); TEST_ASSERT_EQUAL(ALARM_STATUS_ACTIVE,e[0].status);
}
TEST_CASE("duplicate and reverse ingest sequence are ignored", "[environment_alarm]") {
    init(); alarm_environment_sample_t s=sample(9); alarm_engine_diagnostics_t d; TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL)); s.ingest_seq=9;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));s.ingest_seq=8;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_get_diagnostics(&d));TEST_ASSERT_EQUAL_UINT64(1,d.duplicate_sample_count);TEST_ASSERT_EQUAL_UINT64(1,d.out_of_order_sample_count);
}
TEST_CASE("critical air quality suppresses warning", "[environment_alarm]") {
    init(); alarm_environment_sample_t s=sample(1);s.air_quality_score=20;s.air_quality_level=ALARM_AIR_QUALITY_BAD;alarm_event_t e[2];TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));s_now=30000;s.ingest_seq=2;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));TEST_ASSERT_EQUAL_UINT32(1,alarm_engine_peek_events(e,2));TEST_ASSERT_EQUAL(ALARM_TYPE_AIR_QUALITY_CRITICAL,e[0].alarm_type);
}
TEST_CASE("non READY sample cannot recover active critical air quality", "[environment_alarm]") {
    init(); alarm_environment_sample_t s=sample(1);s.air_quality_score=20;s.air_quality_level=ALARM_AIR_QUALITY_BAD;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));s_now=30000;s.ingest_seq=2;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));s_now=60000;s.ingest_seq=3;s.sensor_state=ALARM_SENSOR_WARMUP;s.air_quality_score=90;s.air_quality_level=ALARM_AIR_QUALITY_GOOD;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));alarm_active_alarm_t active[2];TEST_ASSERT_GREATER_THAN_UINT32(0,alarm_engine_get_active(ALARM_DEVICE_C51,active,2));
}
TEST_CASE("event acknowledgement does not clear active state", "[environment_alarm]") {
    init();alarm_environment_sample_t s=sample(1);s.temperature=36;alarm_event_t e;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));s_now=30000;s.ingest_seq=2;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));s_now=60000;s.ingest_seq=3;TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_update(&s,NULL));TEST_ASSERT_EQUAL_UINT32(1,alarm_engine_peek_events(&e,1));TEST_ASSERT_EQUAL(ESP_OK,alarm_engine_ack_events(e.event_seq));alarm_active_alarm_t a[2];TEST_ASSERT_GREATER_THAN_UINT32(0,alarm_engine_get_active(ALARM_DEVICE_C51,a,2));
}
