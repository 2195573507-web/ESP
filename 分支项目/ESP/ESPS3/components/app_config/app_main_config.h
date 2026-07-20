#ifndef APP_MAIN_CONFIG_H
#define APP_MAIN_CONFIG_H

/**
 * @file app_main_config.h
 * @brief S3 gateway startup task configuration.
 */

#ifndef MAIN_IDLE_DELAY_MS
#define MAIN_IDLE_DELAY_MS 1000
#endif

#ifndef APP_STARTUP_TASK_STACK
#define APP_STARTUP_TASK_STACK 8192U
#endif

#ifndef APP_STARTUP_TASK_PRIORITY
#define APP_STARTUP_TASK_PRIORITY 4U
#endif

#if MAIN_IDLE_DELAY_MS <= 0
#error "MAIN_IDLE_DELAY_MS must be greater than 0"
#endif

#if APP_STARTUP_TASK_STACK < 6144
#error "APP_STARTUP_TASK_STACK must be at least 6144"
#endif

#if APP_STARTUP_TASK_PRIORITY <= 0
#error "APP_STARTUP_TASK_PRIORITY must be greater than 0"
#endif

#endif /* APP_MAIN_CONFIG_H */
