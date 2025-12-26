#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_manager_on_got_ip_cb_t)(const esp_netif_ip_info_t *ip, void *user_ctx);
typedef void (*wifi_manager_on_disconnected_cb_t)(void *user_ctx);

typedef struct {
    // Button reset (hold to clear creds + go AP)
    int reset_button_gpio;          // default: 41
    int reset_hold_time_ms;         // default: 10000

    // AP portal config
    const char *ap_ssid;            // default: "ESP32_Setup"
    const char *ap_password;        // default: "" (open)
    bool ap_open_auth;              // default: true
    int ap_max_conn;                // default: 4

    // Webserver config
    int http_port;                  // default: 80
    int http_ctrl_port;             // default: 32768
    int http_max_body;              // default: 512
    const char *ap_ip_str;          // default: "192.168.4.1" (chỉ để trả JSON)

    // STA reconnect behavior
    int sta_max_retries;            // default: 5 (sau đó restart)

    // NVS storage
    const char *nvs_namespace;      // default: "storage"
    const char *nvs_key_ssid;       // default: "wifi_ssid"
    const char *nvs_key_pass;       // default: "wifi_pass"
} wifi_manager_cfg_t;

/**
 * @brief Init Wi-Fi manager:
 *  - Load creds from NVS
 *  - If have SSID -> start STA
 *  - Else -> start AP + web portal
 *  - Start reset button task
 *
 * Note: app_main phải gọi trước:
 *  - nvs_flash_init()
 *  - esp_netif_init()
 *  - esp_event_loop_create_default()
 */
esp_err_t wifi_manager_init(const wifi_manager_cfg_t *cfg);

/** Register callbacks (optional) */
void wifi_manager_register_on_got_ip(wifi_manager_on_got_ip_cb_t cb, void *user_ctx);
void wifi_manager_register_on_disconnected(wifi_manager_on_disconnected_cb_t cb, void *user_ctx);

/** Status */
bool wifi_manager_is_connected(void);

/** Get STA netif handle (để main/ethernet set default route khi failover) */
esp_netif_t *wifi_manager_get_sta_netif(void);

/** Force clear creds and switch to AP portal (restart) */
void wifi_manager_force_ap_portal(void);

#ifdef __cplusplus
}
#endif
