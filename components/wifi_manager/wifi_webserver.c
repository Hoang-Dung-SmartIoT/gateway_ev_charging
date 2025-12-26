#include "wifi_webserver.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/param.h>

static const char *TAG = "wifi_webserver";

typedef struct {
    wifi_webserver_cfg_t cfg;
    httpd_handle_t server;
} ws_ctx_t;

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
        "          status.innerHTML = 'Đã kết nối thành công tới ' + j.ip + '. ESP sẽ rời AP.';\n"
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
    ws_ctx_t *ws = (ws_ctx_t *)req->user_ctx;
    const int BUF_SIZE = ws->cfg.max_body_len > 128 ? ws->cfg.max_body_len : 512;

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

    ESP_LOGI(TAG, "Got creds: ssid=%s pass_len=%d", ssid, (int)strlen(pass));

    esp_err_t save_ok = ESP_FAIL;
    if (ws->cfg.save_creds_cb) {
        save_ok = ws->cfg.save_creds_cb(ssid, pass, ws->cfg.user_ctx);
    }

    cJSON *resp = cJSON_CreateObject();
    if (save_ok == ESP_OK) {
        cJSON_AddBoolToObject(resp, "success", true);
        cJSON_AddStringToObject(resp, "ip", ws->cfg.ap_ip_str ? ws->cfg.ap_ip_str : "192.168.4.1");
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

    if (save_ok == ESP_OK && ws->cfg.after_save_cb) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        ws->cfg.after_save_cb(ws->cfg.user_ctx);
    }
    return ESP_OK;
}

esp_err_t wifi_webserver_start(const wifi_webserver_cfg_t *cfg, httpd_handle_t *out_server)
{
    if (!cfg || !out_server) return ESP_ERR_INVALID_ARG;

    ws_ctx_t *ws = (ws_ctx_t *)calloc(1, sizeof(ws_ctx_t));
    if (!ws) return ESP_ERR_NO_MEM;
    ws->cfg = *cfg;

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = cfg->server_port ? cfg->server_port : 80;
    hc.ctrl_port   = cfg->ctrl_port   ? cfg->ctrl_port   : 32768;

    if (httpd_start(&ws->server, &hc) != ESP_OK) {
        free(ws);
        return ESP_FAIL;
    }

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=root_get_handler, .user_ctx=ws};
    httpd_uri_t conn = {.uri="/connect", .method=HTTP_POST, .handler=connect_post_handler, .user_ctx=ws};

    httpd_register_uri_handler(ws->server, &root);
    httpd_register_uri_handler(ws->server, &conn);

    *out_server = ws->server;
    return ESP_OK;
}

void wifi_webserver_stop(httpd_handle_t server)
{
    if (server) httpd_stop(server);
}
