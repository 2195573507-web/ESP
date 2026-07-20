#ifndef ENVIRONMENT_ALARM_HOST_COMPAT_H
#define ENVIRONMENT_ALARM_HOST_COMPAT_H

#include <stddef.h>

static inline size_t environment_alarm_test_strlcpy(char *destination,
                                                    const char *source,
                                                    size_t size)
{
    size_t source_length = 0U;
    if (source == NULL) {
        source = "";
    }
    while (source[source_length] != '\0') {
        ++source_length;
    }
    if (size > 0U) {
        size_t count = source_length < size - 1U ? source_length : size - 1U;
        for (size_t index = 0U; index < count; ++index) {
            destination[index] = source[index];
        }
        destination[count] = '\0';
    }
    return source_length;
}

#define strlcpy environment_alarm_test_strlcpy

#endif /* ENVIRONMENT_ALARM_HOST_COMPAT_H */
