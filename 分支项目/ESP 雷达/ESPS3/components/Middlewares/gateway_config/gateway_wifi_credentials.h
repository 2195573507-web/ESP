#ifndef GATEWAY_WIFI_CREDENTIALS_H
#define GATEWAY_WIFI_CREDENTIALS_H

/**
 * @file gateway_wifi_credentials.h
 * @brief S3 STA 上游 WiFi 凭据列表。
 *
 * 修改这里即可更换或增加 S3 可连接的上游 WiFi。
 * SSID 最多 32 字节；WPA/WPA2 密码通常为 8-63 字节，开放网络密码填 ""。
 *
 * 直接在 gateway_wifi_sta_credentials[] 中增删条目即可：
 *     { "HomeWiFi", "home-password" },
 *     { "OfficeWiFi", "office-password" },
 */

#include "gateway_config.h"

#ifndef GATEWAY_CONFIG_STA_SSID
#define GATEWAY_CONFIG_STA_SSID ""
#endif

#ifndef GATEWAY_CONFIG_STA_PASSWORD
#define GATEWAY_CONFIG_STA_PASSWORD ""
#endif

static const gateway_wifi_credential_t gateway_wifi_sta_credentials[] = {
    { GATEWAY_CONFIG_STA_SSID, GATEWAY_CONFIG_STA_PASSWORD },
    {"iPhoneWang", "Wlsz060501"},
    {"Matepad","12345678"}
    /* 在这里继续添加可连接 WiFi，例如：
     * { "HomeWiFi", "home-password" },
     * { "OfficeWiFi", "office-password" },
     */
};

#define GATEWAY_WIFI_STA_CREDENTIAL_COUNT \
    (sizeof(gateway_wifi_sta_credentials) / sizeof(gateway_wifi_sta_credentials[0]))

#endif /* GATEWAY_WIFI_CREDENTIALS_H */
