#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

#include <stddef.h>

/**
 * @brief 单个已知 WiFi 的账号信息。
 *
 * 调用方法：在 WIFI_KNOWN_LIST 中新增 {"WiFi名", "WiFi密码"}，
 * wifi_manager 会扫描附近 AP，并自动选择列表中 RSSI 最强的一项连接。
 */
typedef struct {
    const char *ssid;      // WiFi 名称，必须和扫描到的 SSID 完全一致，大小写敏感。
    const char *password;  // WiFi 密码，随固件写入 flash，掉电不丢。
} known_wifi_t;

/*
 * 已知 WiFi 列表
 *
 * 调试说明：
 * 1. 每一行是一组 WiFi 名和密码，格式为 {"WiFi名", "WiFi密码"}。
 * 2. 想加入新的 WiFi，只需要在 WIFI_KNOWN_LIST 里新增一行。
 * 3. 当前连接逻辑会扫描周围 AP，并从这个列表里选择 RSSI 最强的已知 WiFi。
 * 4. 这些数据会随固件写入 ESP32-C5 flash，掉电不会丢失。
 * 5. 如果要在运行时通过串口/网页新增 WiFi 并掉电保存，后续应改成 NVS 存储。
 */
static const known_wifi_t WIFI_KNOWN_LIST[] = {
    {"iPhoneWang", "Wlsz060501"},
    {"LAPTOP-0S4K2TS5 9908","123456780"},
    {"Xiaomi_3818", "hf13597842888"}
    //{"EXAMPLE_WIFI_SSID_2", "EXAMPLE_WIFI_PASSWORD_2"}
};

static const size_t WIFI_KNOWN_COUNT = sizeof(WIFI_KNOWN_LIST) / sizeof(WIFI_KNOWN_LIST[0]);

#endif // WIFI_CREDENTIALS_H
