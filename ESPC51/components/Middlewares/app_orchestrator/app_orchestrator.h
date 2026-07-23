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
 * 调用时机：app_main 创建启动任务后立即调用；本函数只安排 LCD bootstrap 与异步续跑任务后返回，
 *           以释放启动栈。续跑任务在收到 LCD READY 或 FAILED 的有界同步结果后初始化后台服务。
 * 输入参数：无。
 * 返回值：无；本函数不返回正常业务结果。
 * 失败处理：WiFi/网关首次初始化失败进入可观察降级并由已有后台状态机重连；system/BME/
 * voice/radar 的失败由各模块记录日志并保持可恢复或降级运行，不改变对外协议。
 */
void app_orchestrator_start(void);
