/*
 * wifi_bridge.h — 供 Arduino setup()/loop() 调用的 IDF 原生 WiFi 层
 *
 * 设计要点:
 *   1. WiFi 由 IDF API 直接管理，不经过 Arduino 的 WiFi.h，
 *      避免双重 esp_wifi_init() 导致 ESP_ERR_WIFI_INIT_STATE
 *   2. 单核调度下 (CONFIG_FREERTOS_UNICORE=y) 事件回调与用户任务同核，
 *      回调内严禁阻塞
 *   3. 芯片 rev1.0 + eFuse MAC CRC 损坏场景已内建兜底
 *   4. 提供 APSTA API；应用自行实现配网页面及关闭 AP 策略
 */

#ifndef WIFI_BRIDGE_H
#define WIFI_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 当前状态 */
typedef enum {
    WIFI_BR_STATE_IDLE = 0,
    WIFI_BR_STATE_STARTED,
    WIFI_BR_STATE_CONNECTED,
    WIFI_BR_STATE_GOT_IP,
    WIFI_BR_STATE_FAILED,
} wifi_br_state_t;

/* ---------------- STA 模式 ---------------- */

/*
 * 初始化并启动 WiFi STA。
 * 传入 ssid 为 NULL 或空串时仅启动协议栈，不连接（用于扫描验证）。
 * 必须在 Arduino setup() 中调用一次。
 */
esp_err_t wifi_bridge_start(const char *ssid, const char *password);

/*
 * 动态设置 STA 凭据并发起连接。
 * 用于 AP 配网场景：拿到用户提交的 SSID/密码后调用。
 * 保留当前 APSTA 模式；需要关闭 AP 时显式调用 wifi_bridge_ap_stop()。
 * 从普通任务串行调用，不可从事件回调调用。
 */
esp_err_t wifi_bridge_set_sta_credentials(const char *ssid, const char *password);

/* 主动发起扫描，结果通过日志输出，返回发现的 AP 数量；失败返回 -1 */
int wifi_bridge_scan(int max_ap);

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t secure;
} wifi_bridge_ap_t;

#define WIFI_BRIDGE_SCAN_MAX 16

/*
 * 发起非阻塞扫描，结果在 SCAN_DONE 事件中缓存（同名 SSID 去重）。
 * STA 正在连接时驱动会拒绝扫描并返回错误。
 */
esp_err_t wifi_bridge_scan_begin(void);

/* 取走扫描结果：-2 表示仍在扫描，-1 表示没有结果，否则返回复制条数 */
int wifi_bridge_scan_take(wifi_bridge_ap_t *out, int max);

/* 查询当前状态 */
wifi_br_state_t wifi_bridge_state(void);

/* 是否已获取 IP */
bool wifi_bridge_has_ip(void);

/* 获取当前 IPv4 地址字符串，未获取时返回 "0.0.0.0" */
const char *wifi_bridge_ip_str(void);

/* 打印芯片身份与 WiFi 配置摘要 */
void wifi_bridge_dump_info(void);

/* ---------------- AP 配网模式 ---------------- */

/*
 * 启动 SoftAP 配网热点。
 * ssid/password 为 NULL 时使用默认值 (ESP32-CONFIG / 12345678)。
 * ap_password 至少 8 字符，否则退化为开放网络。
 */
esp_err_t wifi_bridge_ap_start(const char *ap_ssid, const char *ap_password);

/* 关闭 AP，切回纯 STA 模式 */
esp_err_t wifi_bridge_ap_stop(void);

/* 是否处于 AP 模式 (含 APSTA) */
bool wifi_bridge_ap_active(void);

/* 当前 AP 的 IP 字符串，未开启时返回 "0.0.0.0" */
const char *wifi_bridge_ap_ip_str(void);

/* 已连接到 AP 的客户端数量 */
int wifi_bridge_ap_sta_count(void);

/* ---------------- 状态枚举转字符串 ---------------- */

const char *wifi_bridge_state_name(wifi_br_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_BRIDGE_H */
