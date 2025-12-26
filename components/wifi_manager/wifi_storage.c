#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include <string.h>

// API nội bộ dùng trong wifi_manager.c
esp_err_t wifi_storage_load(const char *ns, const char *k_ssid, const char *k_pass,
                            char *ssid, size_t ssid_len,
                            char *pass, size_t pass_len)
{
    if (!ns || !k_ssid || !k_pass || !ssid || !pass) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t s_len = ssid_len;
    size_t p_len = pass_len;

    esp_err_t e1 = nvs_get_str(h, k_ssid, ssid, &s_len);
    esp_err_t e2 = nvs_get_str(h, k_pass, pass, &p_len);

    nvs_close(h);

    // Nếu pass không có vẫn OK (pass rỗng)
    if (e1 != ESP_OK) {
        ssid[0] = '\0';
        pass[0] = '\0';
        return e1;
    }
    if (e2 != ESP_OK) {
        pass[0] = '\0';
    }
    // đảm bảo null-terminate
    ssid[ssid_len - 1] = '\0';
    pass[pass_len - 1] = '\0';
    return ESP_OK;
}

esp_err_t wifi_storage_save(const char *ns, const char *k_ssid, const char *k_pass,
                            const char *ssid, const char *pass)
{
    if (!ns || !k_ssid || !k_pass || !ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    esp_err_t e1 = nvs_set_str(h, k_ssid, ssid);
    esp_err_t e2 = nvs_set_str(h, k_pass, pass);
    esp_err_t e3 = nvs_commit(h);

    nvs_close(h);

    if (e1 != ESP_OK) return e1;
    if (e2 != ESP_OK) return e2;
    return e3;
}

esp_err_t wifi_storage_clear(const char *ns, const char *k_ssid, const char *k_pass)
{
    if (!ns || !k_ssid || !k_pass) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_erase_key(h, k_ssid);
    nvs_erase_key(h, k_pass);
    esp_err_t e3 = nvs_commit(h);

    nvs_close(h);
    return e3;
}
