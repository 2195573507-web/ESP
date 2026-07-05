#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* WiFi 重连参数：调扫描间隔、首次连接等待和任务资源时改这里。 */
#define WIFI_RESCAN_DELAY_MS          3000  // 找不到 WiFi 后的重扫间隔。
#define WIFI_CONNECT_TIMEOUT_MS       15000 // 首次连接等待超时。
#define WIFI_RECONNECT_TASK_STACK     3072  // 重连任务栈大小；只做扫描/连接状态机，避免 4096 过度占用 RAM。
#define WIFI_RECONNECT_TASK_PRIORITY  5     // 重连任务优先级。
#define WIFI_STABLE_REQUIRED_MS       3000  // 认为 WiFi 稳定所需的连续连接时长。

/**
 * @brief 初始化Wi-Fi管理功能
 *
 * 调用方法：app_main() 启动早期调用一次，再调用 wifi_connect_to_ap()。
 *
 * 此函数负责初始化NVS、TCP/IP适配器、事件循环和Wi-Fi。
 * @return 成功返回 ESP_OK，失败返回对应错误码。
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief 持续扫描并连接到已保存列表里信号最强的 Wi-Fi AP
 *
 * 调用方法：wifi_manager_init() 成功后调用；本函数会等待首次连接成功。
 *
 * 本函数会启动后台重连任务，并阻塞等待首次连接成功。后台任务会持续扫描
 * 周围 AP，从 wifi_credentials.h 的 WIFI_KNOWN_LIST 中选择 RSSI 最大的一项
 * 连接；如果暂时没有可用 WiFi，会间隔重扫直到连接成功。运行中发生断线时，
 * 后台任务也会重新扫描并自动连接当前可用的已知 WiFi。
 * 新增 WiFi 名和密码时，只需要在 WIFI_KNOWN_LIST 里新增
 * {"WiFi名", "WiFi密码"}。
 * @return 首次连接成功返回 ESP_OK；初始化状态异常或任务创建失败时返回错误码。
 */
esp_err_t wifi_connect_to_ap(void);

/**
 * @brief 读取当前已连接的 WiFi 名称
 *
 * 调用方法：状态页或日志展示当前网络时调用。
 *
 * @param ssid 用来保存 WiFi 名称的缓冲区。
 * @param ssid_len ssid 缓冲区长度，建议至少 33 字节。
 * @return true 表示读取成功，false 表示当前未连接或参数无效。
 */
bool wifi_get_connected_ssid(char *ssid, size_t ssid_len);

/**
 * @brief 获取Wi-Fi连接状态
 *
 * 调用方法：轻量状态判断使用；发起服务器请求前更推荐 wifi_is_stable()。
 *
 * 此函数返回当前连接状态。
 * @return 已连接返回 true，未连接返回 false。
 */
bool wifi_is_connected(void);

/**
 * @brief 判断 WiFi 是否已连接并持续稳定一段时间。
 *
 * 调用方法：需要发起 ESP-server 请求前调用，例如 server voice turn 打开前。
 * 函数会先确认当前已连接，再确认从最近一次 GOT_IP 到现在已经超过
 * WIFI_STABLE_REQUIRED_MS。
 *
 * @return 已连接且稳定返回 true，否则返回 false。
 */
bool wifi_is_stable(void);

#endif // WIFI_MANAGER_H
