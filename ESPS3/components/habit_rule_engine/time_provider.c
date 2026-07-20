#include "time_provider.h"

#include <string.h>

bool habit_time_provider_unavailable(void *context, habit_wall_clock_t *out)
{
    (void)context;
    if (out != NULL) memset(out, 0, sizeof(*out));
    return false;
}
