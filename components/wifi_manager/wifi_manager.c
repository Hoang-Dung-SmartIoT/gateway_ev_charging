#include "wifi_manager.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "driver/gpio.h"

#include "cJSON.h"
#include <sys/param.h>

static const char *TAG = "wifi_manager";

/* ===== storage APIs (implemented in wifi_storage.c) ===== */
esp_err_t wifi_storage_load(const char *ns, const char *k_ssid, const char *k_pass,
                            char *ssid, size_t ssid_len,
                            char *pass, size_t pass_len);
esp_err_t wifi_storage_save(const char *ns, const char *k_ssid, const char *k_pass,
                            const char *ssid, const char *pass);
esp_err_t wifi_storage_clear(const char *ns, const char *k_ssid, const char *k_pass);

/* ===== defaults ===== */
static wifi_manager_cfg_t s_cfg = {
    .reset_button_gpio = 41,
    .reset_hold_time_ms = 10000,
    .ap_ssid = "ESP32_Setup",
    .ap_password = "",
    .ap_open_auth = true,
    .ap_max_conn = 4,
    .http_port = 80,
    .http_ctrl_port = 32768,
    .http_max_body = 512,
    .ap_ip_str = "192.168.4.1",
    .sta_max_retries = 5,
    .nvs_namespace = "storage",
    .nvs_key_ssid = "wifi_ssid",
    .nvs_key_pass = "wifi_pass",
};

static EventGroupHandle_t s_ev = NULL;
static int s_retry = 0;
static httpd_handle_t s_httpd = NULL;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

static wifi_manager_on_got_ip_cb_t s_on_got_ip = NULL;
static void *s_on_got_ip_ctx = NULL;

static wifi_manager_on_disconnected_cb_t s_on_disc = NULL;
static void *s_on_disc_ctx = NULL;

#define WIFI_CONNECTED_BIT BIT0

/* ===================== WEB PORTAL (HTML) ===================== */

static const char *k_html =
    "<!doctype html>\n"
    "<html>\n"
    "<head>\n"
    "  <meta charset='utf-8'>\n"
    "  <meta name='viewport' content='width=device-width, initial-scale=1'>\n"
    "  <title>ESP32 WiFi Setup</title>\n"
    "  <style>\n"
    "    body{font-family: Arial, Helvetica, sans-serif; padding:20px}\n"
    "    .card{max-width:420px;margin:auto;padding:16px;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,0.1)}\n"
    "    input{width:100%;padding:8px;margin:8px 0}\n"
    "    button{padding:10px 18px}\n"
    "    #status{margin-top:12px}\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class='card'>\n"
    "    <h3>ESP32 Wi-Fi Setup</h3>\n"
    "    <p>Nhập SSID và mật khẩu mạng Wi-Fi bạn muốn ESP32 kết nối.</p>\n"
    "    <label>SSID</label>\n"
    "    <input id='ssid' placeholder='Your WiFi SSID'>\n"
    "    <label>Password</label>\n"
    "    <input id='pass' type='password' placeholder='Your WiFi password (can be empty)'>\n"
    "    <button id='btn'>Connect</button>\n"
    "    <div id='status'></div>\n"
    "  </div>\n"
    "  <script>\n"
    "    const btn = document.getElementById('btn');\n"
    "    const status = document.getElementById('status');\n"
    "    btn.onclick = async () => {\n"
    "      const ssid = document.getElementById('ssid').value;\n"
    "      const password = document.getElementById('pass').value;\n"
    "      if(!ssid){ status.innerText='Vui lòng nhập SSID'; return; }\n"
    "      status.innerText = 'Sending...';\n"
    "      try{\n"
    "        const res = await fetch('/connect', {\n"
    "          method:'POST',\n"
    "          headers: {'Content-Type':'application/json'},\n"
    "          body: JSON.stringify({ssid,password})\n"
    "        });\n"
    "        const j = await res.json();\n"
    "        if(j.success){\n"
    "          status.innerHTML = 'Đã lưu cấu hình. Thiết bị sẽ khởi động lại.';\n"
    "        } else {\n"
    "          status.innerText = 'Kết nối thất bại: ' + (j.error || 'unknown');\n"
    "        }\n"
    "      } catch(e){ status.innerText = 'Lỗi mạng: ' + e; }\n"
    "    }\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, k_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    const int BUF_SIZE = (s_cfg.http_max_body > 128) ? s_cfg.http_max_body : 512;
    char *buf = (char *)malloc(BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc fail");
        return ESP_FAIL;
    }
    memset(buf, 0, BUF_SIZE);

    int received = 0;
    int remaining = req->content_len;
    while (remaining > 0 && received < BUF_SIZE - 1) {
        int ret = httpd_req_recv(req, buf + received, MIN(remaining, BUF_SIZE - 1 - received));
        if (ret <= 0) {
            free(buf);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    const cJSON *ssid_json = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *pass_json = cJSON_GetObjectItemCaseSensitive(json, "password");

    if (!cJSON_IsString(ssid_json) || !ssid_json->valuestring || ssid_json->valuestring[0] == '\0') {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID missing");
        return ESP_FAIL;
    }

    char ssid[64] = {0};
    char pass[128] = {0};
    strncpy(ssid, ssid_json->valuestring, sizeof(ssid) - 1);
    if (cJSON_IsString(pass_json) && pass_json->valuestring) {
        strncpy(pass, pass_json->valuestring, sizeof(pass) - 1);
    }

    ESP_LOGI(TAG, "Portal received: ssid=%s pass_len=%d", ssid, (int)strlen(pass));

    esp_err_t save_ok = wifi_storage_save(s_cfg.nvs_namespace, s_cfg.nvs_key_ssid, s_cfg.nvs_key_pass, ssid, pass);

    cJSON *resp = cJSON_CreateObject();
    if (save_ok == ESP_OK) {
        cJSON_AddBoolToObject(resp, "success", true);
        cJSON_AddStringToObject(resp, "ip", s_cfg.ap_ip_str ? s_cfg.ap_ip_str : "192.168.4.1");
    } else {
        cJSON_AddBoolToObject(resp, "success", false);
        cJSON_AddStringToObject(resp, "error", "save_failed");
    }

    char *resp_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    free(resp_str);
    cJSON_Delete(resp);
    cJSON_Delete(json);

    if (save_ok == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    return ESP_OK;
}

static void webserver_start(void)
{
    if (s_httpd) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = s_cfg.http_port ? s_cfg.http_port : 80;
    config.ctrl_port   = s_cfg.http_ctrl_port ? s_cfg.http_ctrl_port : 32768;

    if (httpd_start(&s_httpd, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_uri_t connect_uri = {
            .uri = "/connect",
            .method = HTTP_POST,
            .handler = connect_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_httpd, &root_uri);
        httpd_register_uri_handler(s_httpd, &connect_uri);
        ESP_LOGI(TAG, "Web portal started on port %d", config.server_port);
    } else {
        ESP_LOGW(TAG, "Failed to start HTTP server");
    }
}

/* ===================== WIFI START (STA/AP) ===================== */

static void wifi_start_sta(const char *ssid, const char *pass)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (pass) strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA start: SSID=%s", ssid);
}

static void wifi_start_ap_portal(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, s_cfg.ap_ssid ? s_cfg.ap_ssid : "ESP32_Setup",
            sizeof(ap_config.ap.ssid) - 1);

    if (!s_cfg.ap_open_auth && s_cfg.ap_password && s_cfg.ap_password[0]) {
        strncpy((char *)ap_config.ap.password, s_cfg.ap_password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.password[0] = '\0';
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ap_config.ap.max_connection = (s_cfg.ap_max_conn > 0) ? s_cfg.ap_max_conn : 4;
    ap_config.ap.ssid_len = 0;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP portal started: SSID=%s auth=%s",
             (char *)ap_config.ap.ssid,
             (ap_config.ap.authmode == WIFI_AUTH_OPEN) ? "OPEN" : "WPA2");

    webserver_start();
}

/* ===================== EVENT HANDLER ===================== */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            ESP_LOGI(TAG, "STA started, connecting...");
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "STA disconnected");
            xEventGroupClearBits(s_ev, WIFI_CONNECTED_BIT);

            if (s_on_disc) s_on_disc(s_on_disc_ctx);

            if (s_retry < s_cfg.sta_max_retries) {
                esp_wifi_connect();
                s_retry++;
                ESP_LOGI(TAG, "Retry Wi-Fi (%d/%d)", s_retry, s_cfg.sta_max_retries);
            } else {
                ESP_LOGE(TAG, "Wi-Fi retry exceeded -> restart");
                esp_restart();
            }
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA GOT IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;

        xEventGroupSetBits(s_ev, WIFI_CONNECTED_BIT);

        if (s_on_got_ip) {
            s_on_got_ip(&event->ip_info, s_on_got_ip_ctx);
        }
    }
}

/* ===================== RESET BUTTON TASK ===================== */

static void reset_button_task(void *pv)
{
    gpio_config_t io_conf_in = {
        .pin_bit_mask = (1ULL << (uint64_t)s_cfg.reset_button_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf_in);

    TickType_t press_start = 0;
    bool pressed = false;

    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Reset button GPIO%d start level=%d",
             s_cfg.reset_button_gpio, gpio_get_level(s_cfg.reset_button_gpio));

    while (1) {
        int level = gpio_get_level(s_cfg.reset_button_gpio);

        if (level == 0 && !pressed) {
            pressed = true;
            press_start = xTaskGetTickCount();
        } else if (level == 1 && pressed) {
            pressed = false;
        }

        if (pressed) {
            TickType_t elapsed = xTaskGetTickCount() - press_start;
            if (elapsed >= pdMS_TO_TICKS(s_cfg.reset_hold_time_ms)) {
                ESP_LOGW(TAG, "Button held -> clear creds and AP portal");
                wifi_storage_clear(s_cfg.nvs_namespace, s_cfg.nvs_key_ssid, s_cfg.nvs_key_pass);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart(); // restart để clean state
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ===================== PUBLIC API ===================== */

esp_err_t wifi_manager_init(const wifi_manager_cfg_t *cfg)
{
    if (cfg) s_cfg = *cfg; // copy override config

    if (!s_ev) s_ev = xEventGroupCreate();
    if (!s_ev) return ESP_ERR_NO_MEM;

    // register handlers (1 lần)
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // load creds
    char ssid[64] = {0};
    char pass[128] = {0};

    esp_err_t e = wifi_storage_load(s_cfg.nvs_namespace, s_cfg.nvs_key_ssid, s_cfg.nvs_key_pass,
                                   ssid, sizeof(ssid), pass, sizeof(pass));

    // start reset button task
    xTaskCreate(reset_button_task, "wifi_reset_btn", 3072, NULL, 5, NULL);

    if (e == ESP_OK && ssid[0] != '\0') {
        ESP_LOGI(TAG, "Found saved SSID: %s", ssid);
        wifi_start_sta(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi -> start AP portal");
        wifi_start_ap_portal();
    }
    return ESP_OK;
}

void wifi_manager_register_on_got_ip(wifi_manager_on_got_ip_cb_t cb, void *user_ctx)
{
    s_on_got_ip = cb;
    s_on_got_ip_ctx = user_ctx;
}

void wifi_manager_register_on_disconnected(wifi_manager_on_disconnected_cb_t cb, void *user_ctx)
{
    s_on_disc = cb;
    s_on_disc_ctx = user_ctx;
}

bool wifi_manager_is_connected(void)
{
    if (!s_ev) return false;
    return (xEventGroupGetBits(s_ev) & WIFI_CONNECTED_BIT) != 0;
}

esp_netif_t *wifi_manager_get_sta_netif(void)
{
    return s_sta_netif;
}

void wifi_manager_force_ap_portal(void)
{
    wifi_storage_clear(s_cfg.nvs_namespace, s_cfg.nvs_key_ssid, s_cfg.nvs_key_pass);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}
