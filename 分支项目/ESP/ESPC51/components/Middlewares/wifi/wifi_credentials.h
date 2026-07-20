#ifndef WIFI_CREDENTIALS_H
#define WIFI_CREDENTIALS_H

/*
 * C5 terminal 正式模式只连接 ESPS3 SoftAP。
 *
 * 家庭 WiFi 凭据不再维护在 C5 固件源码里；运行时差异由 NVS 的
 * terminal_cfg 命名空间保存：gateway_ssid、gateway_pass、gateway_ip。
 * 缺省值定义在 terminal_config.h，并且 ESPC51/ESPC52 保持同一套默认。
 */

#endif /* WIFI_CREDENTIALS_H */
