#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ota_manager_status_cb_t)(const char *request_id,
                                        const char *state,
                                        const char *current_version,
                                        const char *available_version,
                                        const char *detail);

/** Register the callback used to publish OTA progress/status. */
void ota_manager_init(ota_manager_status_cb_t status_cb);

/**
 * Start an asynchronous manifest check and, when newer, an HTTPS OTA update.
 * manifest_url may be NULL/empty to use CONFIG_GATEWAY_OTA_MANIFEST_URL.
 */
esp_err_t ota_manager_start(const char *manifest_url,
                            const char *request_id,
                            bool force_update);

/** Mark a pending image valid after the application has regained network access. */
esp_err_t ota_manager_confirm_running_image(void);

bool ota_manager_is_busy(void);
const char *ota_manager_current_version(void);

#ifdef __cplusplus
}
#endif
