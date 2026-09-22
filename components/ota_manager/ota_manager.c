#include "ota_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define OTA_MANIFEST_MAX_BYTES 2048
#define OTA_URL_MAX_LEN         512
#define OTA_VERSION_MAX_LEN     32
#define OTA_REQUEST_ID_MAX_LEN  64

typedef struct {
    char manifest_url[OTA_URL_MAX_LEN];
    char request_id[OTA_REQUEST_ID_MAX_LEN];
    bool force_update;
} ota_job_t;

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
} http_buffer_t;

typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
} semver_t;

static const char *TAG = "ota_manager";
static ota_manager_status_cb_t s_status_cb;
static bool s_busy;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void report(const ota_job_t *job, const char *state,
                   const char *available_version, const char *detail)
{
    ESP_LOGI(TAG, "state=%s current=%s available=%s detail=%s",
             state, ota_manager_current_version(),
             available_version ? available_version : "",
             detail ? detail : "");
    if (s_status_cb) {
        s_status_cb(job ? job->request_id : "", state,
                    ota_manager_current_version(),
                    available_version ? available_version : "",
                    detail ? detail : "");
    }
}

static bool is_https_url(const char *url)
{
    return url && strncmp(url, "https://", 8) == 0;
}

static esp_err_t wait_for_valid_system_time(const ota_job_t *job)
{
    const time_t minimum_valid_epoch = 1700000000; /* 2023-11-14 */
    if (time(NULL) >= minimum_valid_epoch) {
        return ESP_OK;
    }

    report(job, "waiting_for_time", NULL, "sntp_not_synced");
    for (int i = 0; i < 60; ++i) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (time(NULL) >= minimum_valid_epoch) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

static bool parse_semver(const char *text, semver_t *out)
{
    if (!text || !out) {
        return false;
    }
    if (*text == 'v' || *text == 'V') {
        ++text;
    }

    char tail = '\0';
    int count = sscanf(text, "%u.%u.%u%c",
                       &out->major, &out->minor, &out->patch, &tail);
    if (count == 3) {
        return true;
    }

    out->patch = 0;
    count = sscanf(text, "%u.%u%c", &out->major, &out->minor, &tail);
    return count == 2;
}

static int compare_versions(const char *left, const char *right, bool *valid)
{
    semver_t a = {0};
    semver_t b = {0};
    *valid = parse_semver(left, &a) && parse_semver(right, &b);
    if (!*valid) {
        return 0;
    }
    if (a.major != b.major) return a.major > b.major ? 1 : -1;
    if (a.minor != b.minor) return a.minor > b.minor ? 1 : -1;
    if (a.patch != b.patch) return a.patch > b.patch ? 1 : -1;
    return 0;
}

static esp_err_t manifest_http_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    http_buffer_t *buffer = (http_buffer_t *)event->user_data;
    if (!buffer || buffer->overflow) {
        return ESP_FAIL;
    }

    size_t incoming = (size_t)event->data_len;
    if (buffer->length + incoming >= buffer->capacity) {
        buffer->overflow = true;
        return ESP_FAIL;
    }
    memcpy(buffer->data + buffer->length, event->data, incoming);
    buffer->length += incoming;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

static esp_err_t fetch_manifest(const char *url,
                                char *version, size_t version_size,
                                char *firmware_url, size_t firmware_url_size)
{
    char *body = calloc(1, OTA_MANIFEST_MAX_BYTES);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    http_buffer_t response = {
        .data = body,
        .capacity = OTA_MANIFEST_MAX_BYTES,
    };
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = manifest_http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "gateway-charging-station");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300 || response.overflow) {
        ESP_LOGE(TAG, "Manifest request failed: err=%s http=%d overflow=%d",
                 esp_err_to_name(err), status, response.overflow);
        free(body);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *version_json = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *url_json = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(url_json)) {
        url_json = cJSON_GetObjectItemCaseSensitive(root, "firmware_url");
    }

    if (!cJSON_IsString(version_json) || !version_json->valuestring ||
        !cJSON_IsString(url_json) || !url_json->valuestring ||
        !is_https_url(url_json->valuestring) ||
        strlen(version_json->valuestring) >= version_size ||
        strlen(url_json->valuestring) >= firmware_url_size) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    strlcpy(version, version_json->valuestring, version_size);
    strlcpy(firmware_url, url_json->valuestring, firmware_url_size);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t download_firmware(const ota_job_t *job,
                                   const char *firmware_url,
                                   const char *manifest_version)
{
    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .max_redirection_count = 8,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        return err;
    }

    esp_app_desc_t new_app = {0};
    err = esp_https_ota_get_img_desc(handle, &new_app);
    if (err != ESP_OK) {
        esp_https_ota_abort(handle);
        return err;
    }
    if (strcmp(new_app.version, manifest_version) != 0) {
        ESP_LOGE(TAG, "Manifest version %s differs from image version %s",
                 manifest_version, new_app.version);
        esp_https_ota_abort(handle);
        return ESP_ERR_INVALID_VERSION;
    }

    int last_progress = -1;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int total = esp_https_ota_get_image_size(handle);
        int read = esp_https_ota_get_image_len_read(handle);
        if (total > 0 && read >= 0) {
            int progress = (read * 100) / total;
            int bucket = progress / 10;
            if (bucket != last_progress) {
                char detail[32];
                snprintf(detail, sizeof(detail), "%d%%", progress);
                report(job, "downloading", manifest_version, detail);
                last_progress = bucket;
            }
        }
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    return esp_https_ota_finish(handle);
}

static void ota_task(void *arg)
{
    ota_job_t *job = (ota_job_t *)arg;
    char available_version[OTA_VERSION_MAX_LEN] = {0};
    char firmware_url[OTA_URL_MAX_LEN] = {0};

    esp_err_t err = wait_for_valid_system_time(job);
    if (err != ESP_OK) {
        report(job, "error", NULL, "time_sync_timeout");
        goto done;
    }

    report(job, "checking", NULL, "fetching_manifest");
    err = fetch_manifest(job->manifest_url,
                                   available_version, sizeof(available_version),
                                   firmware_url, sizeof(firmware_url));
    if (err != ESP_OK) {
        report(job, "error", NULL, esp_err_to_name(err));
        goto done;
    }

    bool valid_versions = false;
    int comparison = compare_versions(available_version,
                                      ota_manager_current_version(),
                                      &valid_versions);
    if (!valid_versions) {
        report(job, "error", available_version, "invalid_semantic_version");
        goto done;
    }
    if (!job->force_update && comparison <= 0) {
        report(job, "up_to_date", available_version, "no_newer_version");
        goto done;
    }

    report(job, "update_available", available_version,
           job->force_update ? "forced" : "newer_version");
    report(job, "downloading", available_version, "0%");
    err = download_firmware(job, firmware_url, available_version);
    if (err != ESP_OK) {
        report(job, "error", available_version, esp_err_to_name(err));
        goto done;
    }

    report(job, "success", available_version, "restarting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

done:
    taskENTER_CRITICAL(&s_lock);
    s_busy = false;
    taskEXIT_CRITICAL(&s_lock);
    free(job);
    vTaskDelete(NULL);
}

void ota_manager_init(ota_manager_status_cb_t status_cb)
{
    s_status_cb = status_cb;
}

esp_err_t ota_manager_start(const char *manifest_url,
                            const char *request_id,
                            bool force_update)
{
    const char *selected_url = manifest_url;
    if (!selected_url || selected_url[0] == '\0') {
        selected_url = CONFIG_GATEWAY_OTA_MANIFEST_URL;
    }
    if (!is_https_url(selected_url) || strlen(selected_url) >= OTA_URL_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_busy) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    taskEXIT_CRITICAL(&s_lock);

    ota_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        taskENTER_CRITICAL(&s_lock);
        s_busy = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(job->manifest_url, selected_url, sizeof(job->manifest_url));
    if (request_id) {
        strlcpy(job->request_id, request_id, sizeof(job->request_id));
    }
    job->force_update = force_update;

    if (xTaskCreate(ota_task, "ota_manager", 10240, job, 8, NULL) != pdPASS) {
        free(job);
        taskENTER_CRITICAL(&s_lock);
        s_busy = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_manager_confirm_running_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Network restored; confirming running OTA image");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    return ESP_OK;
}

bool ota_manager_is_busy(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool busy = s_busy;
    taskEXIT_CRITICAL(&s_lock);
    return busy;
}

const char *ota_manager_current_version(void)
{
    return esp_app_get_description()->version;
}
