#include <assert.h>
#include <stdio.h>

#include "cJSON.h"
#include "home_ai_weather_context.h"

static bool parse(const char *json, bool *available, bool *dark, uint64_t *expires_at_uptime_ms)
{
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    const bool result = home_ai_weather_context_parse(root,
                                                      10000U,
                                                      5000U,
                                                      available,
                                                      dark,
                                                      expires_at_uptime_ms);
    cJSON_Delete(root);
    return result;
}

int main(void)
{
    bool available = true;
    bool dark = true;
    uint64_t expires = 1U;
    assert(parse("{\"schema_version\":1}", &available, &dark, &expires));
    assert(!available && !dark && expires == 0U);

    assert(parse("{\"weather_context\":{\"available\":false,\"dark\":false,"
                 "\"observed_at_ms\":null,\"expires_at_ms\":null}}",
                 &available,
                 &dark,
                 &expires));
    assert(!available && !dark && expires == 0U);

    assert(!parse("{\"weather_context\":{\"available\":false,\"dark\":false,"
                  "\"observed_at_ms\":\"bad\",\"expires_at_ms\":null}}",
                  &available,
                  &dark,
                  &expires));
    assert(!parse("{\"weather_context\":{\"available\":false,\"dark\":false,"
                  "\"observed_at_ms\":9000,\"expires_at_ms\":8000}}",
                  &available,
                  &dark,
                  &expires));
    assert(!parse("{\"weather_context\":{\"available\":\"false\",\"dark\":false}}",
                  &available,
                  &dark,
                  &expires));

    assert(parse("{\"weather_context\":{\"available\":true,\"dark\":true,"
                 "\"observed_at_ms\":9000,\"expires_at_ms\":12000}}",
                 &available,
                 &dark,
                 &expires));
    assert(available && dark && expires == 7000U);
    assert(!parse("{\"weather_context\":{\"available\":true,\"dark\":true,"
                  "\"observed_at_ms\":9000,\"expires_at_ms\":null}}",
                  &available,
                  &dark,
                  &expires));
    assert(!parse("{\"weather_context\":{\"available\":true,\"dark\":false,"
                  "\"observed_at_ms\":9000,\"expires_at_ms\":10000}}",
                  &available,
                  &dark,
                  &expires));
    assert(!parse("{\"weather_context\":[]}", &available, &dark, &expires));

    puts("home ai weather context host tests: PASS");
    return 0;
}
