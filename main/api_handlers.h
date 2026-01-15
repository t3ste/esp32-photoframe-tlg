// new file for v1.9.0_tlg
#ifndef API_HANDLERS_H
#define API_HANDLERS_H

#include <stdbool.h>  // <- boolean

#include "cJSON.h"
#include "esp_err.h"

// Album operations
esp_err_t api_get_albums(cJSON **response);
esp_err_t api_create_album(const char *name, cJSON **response);
esp_err_t api_delete_album(const char *name, cJSON **response);
esp_err_t api_set_album_enabled(const char *name, bool enabled, cJSON **response);
esp_err_t api_list_album_images(const char *album, cJSON **response);

// Display operations
esp_err_t api_display_image(const char *filename, cJSON **response);

// Image deletion
esp_err_t api_delete_image(const char *filename, cJSON **response);

// Configuration
esp_err_t api_get_config(cJSON **response);
// esp_err_t api_set_config(int rotate_interval, bool auto_rotate, cJSON **response);
esp_err_t api_update_config(cJSON *config, cJSON **response);

// Battery status
esp_err_t api_get_battery(cJSON **response);

// Sleep
// esp_err_t api_trigger_sleep(cJSON **response);

#endif  // API_HANDLERS_H