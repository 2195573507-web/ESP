#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

/**
 * @file wifi_manager.h
 * @brief C5 终端 WiFi 连接接口。
 *
 * 本模块只负责 C5 作为 STA 连接 ESPS3 SoftAP，并给启动编排和 HTTP 层提供
 * connected/stable 状态；不负责 S3 SoftAP 创建，也不负责 Server 上云。
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* WiFi 重连参数：C5 只连接 ESPS3 SoftAP，不扫描家庭 WiFi 列表。 */
#define WIFI_RECONNECT_BACKOFF_MIN_MS 1000  // 断开后最短重连退避，避免快速循环 connect。
#define WIFI_RECONNECT_BACKOFF_MAX_MS 3000  // 连续失败后的最大重连退避。
#define WIFI_RECONNECT_BACKOFF_STEP_MS 1000 // 每次失败增加的退避步长。
#define WIFI_CONNECT_TIMEOUT_MS       15000 // 首次连接等待超时。
#define WIFI_RECONNECT_TASK_STACK     3072  // 重连任务栈大小；只做连接状态机。
#define WIFI_RECONNECT_TASK_PRIORITY  5     // 重连任务优先级。
#define WIFI_STABLE_REQUIRED_MS       3000  // 认为 WiFi 稳定所需的连续连接时长。
#define WIFI_DOWN_STABLE_REQUIRED_MS  1000  // 断开持续一小段时间后才通知上层 DOWN。

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
 * @brief 持续连接到 NVS/默认配置指定的 ESPS3 SoftAP
 *
 * 调用方法：wifi_manager_init() 成功后调用；本函数会等待首次连接成功。
 *
 * 本函数会启动后台重连任务，并阻塞等待首次连接成功。后台任务只使用
 * terminal_config 中的 gateway_ssid/gateway_password，不连接家庭 WiFi。
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
 * 调用方法：需要发起 S3 local gateway 请求前调用，例如 voice turn 打开前。
 * 函数会先确认当前已连接，再确认从最近一次 GOT_IP 到现在已经超过
 * WIFI_STABLE_REQUIRED_MS。
 *
 * @return 已连接且稳定返回 true，否则返回 false。
 */
bool wifi_is_stable(void);

/**
 * @brief 判断 WiFi 是否已持续断开一段时间。
 *
 * 调用方法：gateway_link 后台任务使用；不要在 WiFi 事件回调里做上层状态切换。
 *
 * @return 已断开且超过 WIFI_DOWN_STABLE_REQUIRED_MS 返回 true，否则返回 false。
 */
bool wifi_is_down_stable(void);

#endif // WIFI_MANAGER_H
