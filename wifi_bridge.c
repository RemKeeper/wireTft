/*
 * wifi_bridge.c — IDF 原生 WiFi 层实现
 *
 * 移植自已验证的 esp32-unicore-wifi/main/main.c，改动:
 *   - 移除 app_main(), 改为 wifi_bridge_start() 供 Arduino setup() 调用
 *   - 保留全部单核调度约束与 rev1.0 / eFuse MAC 兜底逻辑
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include "wifi_bridge.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"

static const char *TAG = "WIFI_BRIDGE";

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static wifi_br_state_t s_state = WIFI_BR_STATE_IDLE;
static char s_ip_str[16] = "0.0.0.0";
static char s_ap_ip_str[16] = "0.0.0.0";
static bool s_ap_active = false;
static volatile int s_ap_sta_count = 0;

#define BIT_GOT_IP     BIT0
#define BIT_FAIL       BIT1
#define BIT_SCAN_DONE  BIT2
#define BIT_DISCONNECTED BIT3
#define BIT_STOPPED BIT4
static bool s_reconfiguring = false;
static bool s_connect_enabled = false;

static int s_retry = 0;
static const int s_max_retry = 5;
/* 事件回调中缓存扫描结果: esp_wifi_scan_get_ap_num() 在 SCAN_DONE 后
 * 只保证一次有效, 回调里已消费该结果, 故在此缓存供调用方读取 */
static volatile int s_last_scan_count = -1;
/* wifi_bridge_scan_begin() 的异步结果: 同名 SSID 只保留信号最强的一条 */
static wifi_bridge_ap_t s_scan_list[WIFI_BRIDGE_SCAN_MAX];
static volatile int s_scan_list_count = -1;
static volatile bool s_scan_pending = false;

/* ---------- CPU 频率 (v5.5 公开 API) ---------- */
static uint32_t cpu_freq_mhz(void)
{
    uint32_t f = 0;
    if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                     &f) == ESP_OK) {
        return f / 1000000;
    }
    return 0;
}

/* ---------- eFuse MAC 损坏兜底 ---------- */
/* 蓝牙控制器从基础 MAC 派生地址。eFuse CRC 损坏时，在协议栈启动前把
 * 与 STA 相同的兜底地址设为基础 MAC，STA 地址与设备 ID 保持不变。 */
static void ensure_valid_base_mac(void)
{
    uint8_t base[6] = {0};
    if (esp_efuse_mac_get_default(base) == ESP_OK && !(base[0] & 0x01)) return;
    uint8_t mac[6] = {0x02, 0xE5, base[0], base[1], base[2], base[5]};
    esp_err_t err = esp_iface_mac_addr_set(mac, ESP_MAC_BASE);
    if (err != ESP_OK) ESP_LOGE(TAG, "设置兜底基础 MAC 失败: %s", esp_err_to_name(err));
    else ESP_LOGW(TAG, "eFuse MAC 无效，基础 MAC 使用兜底 " MACSTR, MAC2STR(mac));
}

static void ensure_valid_mac(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    bool invalid = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) { invalid = false; break; }
    }
    if (!invalid && (mac[0] & 0x01)) invalid = true;

    if (!invalid) {
        ESP_LOGI(TAG, "eFuse MAC 正常: " MACSTR, MAC2STR(mac));
        return;
    }

    ESP_LOGE(TAG, "eFuse MAC 无效 (CRC 损坏或未烧写), 启用兜底 MAC");

    uint8_t base[6] = {0};
    esp_efuse_mac_get_default(base);
    uint32_t chipid = ((uint32_t)base[0] << 16) |
                      ((uint32_t)base[1] << 8) | base[2];

    mac[0] = 0x02;              /* 本地管理地址 */
    mac[1] = 0xE5;
    mac[2] = (chipid >> 16) & 0xFF;
    mac[3] = (chipid >>  8) & 0xFF;
    mac[4] = chipid & 0xFF;
    mac[5] = base[5];

    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置兜底 MAC 失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "已应用兜底 MAC = " MACSTR, MAC2STR(mac));
    }
}

/* ---------- 事件处理 (单核下必须极短) ---------- */
static void evt_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "[EVT] STA_START");
            s_state = WIFI_BR_STATE_STARTED;
            /* 连接由 wifi_bridge_start() 显式触发, 此处不重复 */
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            ESP_LOGI(TAG, "[EVT] CONNECTED");
            s_state = WIFI_BR_STATE_CONNECTED;
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "[EVT] DISCONNECTED reason=%d", e->reason);
            strcpy(s_ip_str, "0.0.0.0");
            s_state = WIFI_BR_STATE_IDLE;
            xEventGroupClearBits(s_evt, BIT_GOT_IP);
            xEventGroupSetBits(s_evt, BIT_DISCONNECTED);
            if (s_reconfiguring || !s_connect_enabled) break;
            if (s_retry < s_max_retry) {
                s_retry++;
                ESP_LOGW(TAG, "       重连 %d/%d", s_retry, s_max_retry);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "       重连耗尽");
                s_state = WIFI_BR_STATE_FAILED;
                xEventGroupSetBits(s_evt, BIT_FAIL);
            }
            break;
        }

        case WIFI_EVENT_STA_STOP:
            s_state = WIFI_BR_STATE_IDLE;
            xEventGroupSetBits(s_evt, BIT_STOPPED);
            break;

        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "[EVT] AP_START");
            s_ap_active = true;
            break;

        case WIFI_EVENT_AP_STOP:
            ESP_LOGI(TAG, "[EVT] AP_STOP");
            s_ap_active = false;
            s_ap_sta_count = 0;
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e =
                (wifi_event_ap_staconnected_t *)data;
            s_ap_sta_count++;
            ESP_LOGI(TAG, "[EVT] AP客户端接入 " MACSTR " (共 %d)",
                     MAC2STR(e->mac), s_ap_sta_count);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e =
                (wifi_event_ap_stadisconnected_t *)data;
            if (s_ap_sta_count > 0) s_ap_sta_count--;
            ESP_LOGI(TAG, "[EVT] AP客户端离开 " MACSTR " (剩 %d)",
                     MAC2STR(e->mac), s_ap_sta_count);
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            s_last_scan_count = (int)n;      /* 缓存, 供 wifi_bridge_scan 返回 */
            ESP_LOGI(TAG, "[EVT] SCAN_DONE 发现 %u 个 AP", n);

            int listed = 0;
            if (n > 0) {
                if (n > 30) n = 30;
                wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
                if (recs) {
                    uint16_t got = n;
                    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                        for (int i = 0; i < got; i++) {
                            ESP_LOGI(TAG, "  [%2d] %-30.30s %4d  ch%-2d",
                                     i, (char *)recs[i].ssid,
                                     recs[i].rssi, recs[i].primary);
                            const char *ssid = (const char *)recs[i].ssid;
                            if (!ssid[0]) continue;
                            int k = 0;
                            while (k < listed && strcmp(s_scan_list[k].ssid, ssid)) k++;
                            if (k == listed) {
                                if (listed == WIFI_BRIDGE_SCAN_MAX) continue;
                                strlcpy(s_scan_list[k].ssid, ssid, sizeof(s_scan_list[k].ssid));
                                s_scan_list[k].rssi = -128;
                                listed++;
                            }
                            if (recs[i].rssi > s_scan_list[k].rssi) {
                                s_scan_list[k].rssi = recs[i].rssi;
                                s_scan_list[k].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                            }
                        }
                    }
                    free(recs);
                }
            }
            s_scan_list_count = listed;
            s_scan_pending = false;
            xEventGroupSetBits(s_evt, BIT_SCAN_DONE);
            break;
        }

        default:
            ESP_LOGD(TAG, "[EVT] WIFI id=%" PRId32, id);
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "[EVT] GOT_IP %s", s_ip_str);
        s_retry = 0;
        s_state = WIFI_BR_STATE_GOT_IP;
        xEventGroupSetBits(s_evt, BIT_GOT_IP);
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *e = (ip_event_ap_staipassigned_t *)data;
        ESP_LOGI(TAG, "[EVT] AP已分配IP " IPSTR " 给客户端",
                 IP2STR(&e->ip));
    }
}

/* ---------- 芯片信息 ---------- */
void wifi_bridge_dump_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "================ 芯片识别 ================");
    ESP_LOGI(TAG, "cores       = %d", chip.cores);
    ESP_LOGI(TAG, "revision    = %d", chip.revision);
    ESP_LOGI(TAG, "model       = %d", chip.model);
    ESP_LOGI(TAG, "features    = %s%s%s",
             (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (chip.features & CHIP_FEATURE_BT)       ? "BT "   : "",
             (chip.features & CHIP_FEATURE_BLE)      ? "BLE "  : "");
    ESP_LOGI(TAG, "cpu_freq    = %" PRIu32 " MHz", cpu_freq_mhz());
    ESP_LOGI(TAG, "idf_version = %s", esp_get_idf_version());

#if CONFIG_FREERTOS_UNICORE
    ESP_LOGW(TAG, "编译模式    = UNICORE (单核调度)");
#else
    ESP_LOGW(TAG, "编译模式    = DUALCORE");
#endif

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "STA MAC     = " MACSTR, MAC2STR(mac));
    ESP_LOGI(TAG, "free heap   = %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "==========================================");
}

/* ---------- 启动 (初始化协议栈, 不决定模式) ---------- */
esp_err_t wifi_bridge_start(const char *ssid, const char *password)
{
    if (s_evt) return ESP_ERR_INVALID_STATE;
    if ((ssid && strlen(ssid) > 32) || (password && strlen(password) > 64))
        return ESP_ERR_INVALID_ARG;
    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;
    ensure_valid_base_mac();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.wifi_task_core_id = 0;      /* 单核: 显式绑 core 0 */
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &evt_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &evt_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &evt_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* MAC 兜底必须在 set_mode 之后: esp_wifi_set_mac() 要求
     * 接口模式已确定, 否则返回 ESP_ERR_WIFI_MODE */
    ensure_valid_mac();

    wifi_config_t sc = {0};
    bool do_connect = (ssid && strlen(ssid) > 0);

    if (do_connect) {
        memcpy(sc.sta.ssid, ssid, strlen(ssid));
        if (password) memcpy(sc.sta.password, password, strlen(password));
        sc.sta.threshold.authmode = (password && *password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sc.sta.pmf_cfg.capable  = true;
        sc.sta.pmf_cfg.required = false;
    } else {
        ESP_LOGW(TAG, "未提供 SSID, 仅启动协议栈");
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sc));

    /* esp_wifi_start() 必须在 set_max_tx_power 之前 */
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi 已启动");

    esp_err_t tp = esp_wifi_set_max_tx_power(78);
    if (tp == ESP_OK) {
        int8_t v = 0;
        esp_wifi_get_max_tx_power(&v);
        ESP_LOGI(TAG, "发射功率 %.1f dBm", v * 0.25);
    }

    s_retry = 0;
    s_connect_enabled = do_connect;
    if (do_connect) {
        esp_wifi_connect();
    }
    return ESP_OK;
}

/* ---------- 动态设置 STA 凭据并连接 (AP 配网用) ---------- */
esp_err_t wifi_bridge_set_sta_credentials(const char *ssid, const char *password)
{
    if (!s_evt) return ESP_ERR_INVALID_STATE;
    if (!ssid) return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) > 32) {
        ESP_LOGE(TAG, "SSID 超长 (max 32)");
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) > 64) {
        ESP_LOGE(TAG, "密码超长 (max 64)");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t sc = {0};
    memcpy(sc.sta.ssid, ssid, strlen(ssid));
    if (password) memcpy(sc.sta.password, password, strlen(password));
    sc.sta.threshold.authmode = (password && strlen(password))
                                ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sc.sta.pmf_cfg.capable  = true;
    sc.sta.pmf_cfg.required = false;

    /* STOP is a FIFO event barrier: old disconnect/IP callbacks finish while
     * reconnect is disabled, before a new STA configuration can be started. */
    s_reconfiguring = true;
    s_connect_enabled = false;
    xEventGroupClearBits(s_evt, BIT_STOPPED | BIT_DISCONNECTED | BIT_GOT_IP | BIT_FAIL);
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK || !(xEventGroupWaitBits(s_evt, BIT_STOPPED,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(3000)) & BIT_STOPPED)) {
        s_state = WIFI_BR_STATE_FAILED;
        return err != ESP_OK ? err : ESP_ERR_TIMEOUT;
    }
    s_retry = 0;
    s_state = WIFI_BR_STATE_STARTED;
    s_scan_pending = false;   /* 停止 WiFi 会中断进行中的扫描 */
    strcpy(s_ip_str, "0.0.0.0");
    err = esp_wifi_set_config(WIFI_IF_STA, &sc);
    if (err != ESP_OK) {
        s_reconfiguring = false;
        ESP_LOGE(TAG, "set_config 失败: %s", esp_err_to_name(err));
        return err;
    }

    s_connect_enabled = *ssid != 0;
    s_reconfiguring = false;
    err = esp_wifi_start();
    if (err != ESP_OK) { s_state = WIFI_BR_STATE_FAILED; return err; }
    if (!s_connect_enabled) return ESP_OK;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_state = WIFI_BR_STATE_FAILED;
        ESP_LOGE(TAG, "connect 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "STA configuration applied");
    return ESP_OK;
}

/* ---------- 扫描 ---------- */
int wifi_bridge_scan(int max_ap)
{
    wifi_scan_config_t sc = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    xEventGroupClearBits(s_evt, BIT_SCAN_DONE);
    s_last_scan_count = -1;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        return -1;
    }
    EventBits_t b = xEventGroupWaitBits(s_evt, BIT_SCAN_DONE,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(b & BIT_SCAN_DONE)) return -1;

    /* 使用回调中缓存的数量, 而非再次调用 get_ap_num (会返回 0) */
    return s_last_scan_count;
}

esp_err_t wifi_bridge_scan_begin(void)
{
    if (!s_evt) return ESP_ERR_INVALID_STATE;
    if (s_scan_pending) return ESP_ERR_INVALID_STATE;
    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 80,
        .scan_time.active.max = 200,
    };
    s_scan_list_count = -1;
    s_scan_pending = true;
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) s_scan_pending = false;
    return err;
}

int wifi_bridge_scan_take(wifi_bridge_ap_t *out, int max)
{
    if (s_scan_pending) return -2;
    int n = s_scan_list_count;
    if (n < 0) return -1;
    if (n > max) n = max;
    memcpy(out, s_scan_list, n * sizeof(*out));
    s_scan_list_count = -1;
    return n;
}

wifi_br_state_t wifi_bridge_state(void) { return s_state; }
bool wifi_bridge_has_ip(void) { return s_state == WIFI_BR_STATE_GOT_IP; }
const char *wifi_bridge_ip_str(void) { return s_ip_str; }
const char *wifi_bridge_ap_ip_str(void) { return s_ap_ip_str; }
bool wifi_bridge_ap_active(void) { return s_ap_active; }
int wifi_bridge_ap_sta_count(void) { return s_ap_sta_count; }

const char *wifi_bridge_state_name(wifi_br_state_t s)
{
    switch (s) {
    case WIFI_BR_STATE_IDLE:      return "IDLE";
    case WIFI_BR_STATE_STARTED:   return "STARTED";
    case WIFI_BR_STATE_CONNECTED: return "CONNECTED";
    case WIFI_BR_STATE_GOT_IP:    return "GOT_IP";
    case WIFI_BR_STATE_FAILED:    return "FAILED";
    default:                      return "?";
    }
}

/* ---------- AP 配网模式 ---------- */

#define DEFAULT_AP_SSID  "ESP32-CONFIG"
#define DEFAULT_AP_PASS  "12345678"

esp_err_t wifi_bridge_ap_start(const char *ap_ssid, const char *ap_password)
{
    if (!s_evt) {
        ESP_LOGE(TAG, "AP 启动前必须先调用 wifi_bridge_start()");
        return ESP_ERR_INVALID_STATE;
    }

    const char *ssid = (ap_ssid && strlen(ap_ssid)) ? ap_ssid : DEFAULT_AP_SSID;
    const char *pass = (ap_password && strlen(ap_password))
                       ? ap_password : DEFAULT_AP_PASS;

    /* 创建 AP 网络接口 (仅首次) */
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_FAIL;
    }

    /* 切成 APSTA, 保留 STA 接口以便扫描 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) return err;

    wifi_config_t ac = {0};
    strlcpy((char *)ac.ap.ssid, ssid, sizeof(ac.ap.ssid));
    ac.ap.ssid_len = strlen(ssid);
    ac.ap.channel  = 1;                 /* 固定信道 1, 与 STA 扫描兼容 */
    ac.ap.max_connection = 4;
    ac.ap.beacon_interval = 100;

    if (strlen(pass) >= 8) {
        strlcpy((char *)ac.ap.password, pass, sizeof(ac.ap.password));
        ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ac.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "AP 密码不足 8 位, 退化为开放网络");
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP 配置失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 记录 AP IP 供网页提示 */
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        snprintf(s_ap_ip_str, sizeof(s_ap_ip_str),
                 IPSTR, IP2STR(&ip.ip));
    }

    s_ap_active = true;
    ESP_LOGI(TAG, "配网热点已开启: SSID=%s IP=%s", ssid, s_ap_ip_str);
    return ESP_OK;
}

esp_err_t wifi_bridge_ap_stop(void)
{
    if (!s_ap_active) return ESP_OK;

    /* 先切纯 STA 再停 AP, 避免 STA 连接被中断 */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "切 STA 模式失败: %s", esp_err_to_name(err));
    }
    s_ap_active = false;
    s_ap_sta_count = 0;
    strcpy(s_ap_ip_str, "0.0.0.0");
    ESP_LOGI(TAG, "配网热点已关闭");
    return ESP_OK;
}
