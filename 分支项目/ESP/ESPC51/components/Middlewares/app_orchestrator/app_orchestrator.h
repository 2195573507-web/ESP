#pragma once

/**
 * @file app_orchestrator.h
 * @brief C5 终端启动编排入口。
 *
 * 本头文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），只暴露启动编排入口给
 * main/app_main.c；不暴露 WiFi、BME、voice、command 或 placeholder 的内部函数。
 */

/**
 * @brief 启动 C5 终端的后台服务链路。
 *
 * 调用位置：main/app_startup_task()。
 * 调用时机：app_main 创建启动任务后立即调用，一次调用后函数保持在 idle loop。
 * 输入参数：无。
 * 返回值：无；本函数不返回正常业务结果。
 * 失败处理：WiFi 初始化/首次连接失败会停留在错误等待；system/BME/voice 的失败由
 * 各模块记录日志并保持可恢复或降级运行，不改变对外协议。
 */
void app_orchestrator_start(void);
