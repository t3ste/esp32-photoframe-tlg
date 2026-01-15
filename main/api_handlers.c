// new file for v1.9.0_tlg
#include "api_handlers.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>  // <- for unlink()

#include "album_manager.h"
#include "axp_prot.h"
#include "config.h"
#include "display_manager.h"
#include "esp_log.h"
#include "image_processor.h"  // <- for portrait mode
#include "power_manager.h"
// #include <stdbool.h>

static const char *TAG __attribute__((unused)) = "api_handlers";
// static const char *TAG = "api_handlers";

/*
// simulate low battery
ESP_LOGI(TAG, "Battery: %d%%, USB: %s",
         axp_get_battery_percent(),
         axp_is_usb_connected() ? "YES" : "NO");

*/

// API helper functions
// Get portrait combine status
esp_err_t api_get_portrait_combine(cJSON **response)
{
    *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(*response, "enabled", image_processor_get_portrait_combine_enabled());
    return ESP_OK;
}

// Set portrait combine status
esp_err_t api_set_portrait_combine(cJSON *request, cJSON **response)
{
    cJSON *enabled_obj = cJSON_GetObjectItem(request, "enabled");
    if (!enabled_obj || !cJSON_IsBool(enabled_obj)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool enabled = cJSON_IsTrue(enabled_obj);
    esp_err_t err = image_processor_set_portrait_combine_enabled(enabled);

    *response = cJSON_CreateObject();
    cJSON_AddStringToObject(*response, "status", (err == ESP_OK) ? "success" : "error");
    return err;
}

// Get all albums
esp_err_t api_get_albums(cJSON **response)
{
    char **albums = NULL;
    int count = 0;

    if (album_manager_list_albums(&albums, &count) != ESP_OK) {
        return ESP_FAIL;
    }

    *response = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *album_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(album_obj, "name", albums[i]);
        cJSON_AddBoolToObject(album_obj, "enabled", album_manager_is_album_enabled(albums[i]));
        cJSON_AddItemToArray(*response, album_obj);
    }

    album_manager_free_album_list(albums, count);
    return ESP_OK;
}

// List images in album
esp_err_t api_list_album_images(const char *album, cJSON **response)
{
    char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
    if (album_manager_get_album_path(album, album_path, sizeof(album_path)) != ESP_OK) {
        return ESP_FAIL;
    }

    DIR *dir = opendir(album_path);
    if (!dir) {
        return ESP_FAIL;
    }

    *response = cJSON_CreateArray();
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                continue;  // Skip macOS metadata files
            }
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                cJSON *image_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(image_obj, "name", entry->d_name);
                cJSON_AddItemToArray(*response, image_obj);
            }
        }
    }
    closedir(dir);
    return ESP_OK;
}

// Create album
esp_err_t api_create_album(const char *name, cJSON **response)
{
    esp_err_t err = album_manager_create_album(name);
    if (err != ESP_OK) {
        return err;
    }

    *response = cJSON_CreateObject();
    cJSON_AddStringToObject(*response, "status", "success");
    cJSON_AddStringToObject(*response, "message", "Album created");
    return ESP_OK;
}

// Delete album
esp_err_t api_delete_album(const char *name, cJSON **response)
{
    esp_err_t err = album_manager_delete_album(name);
    if (err != ESP_OK) {
        *response = cJSON_CreateObject();
        cJSON_AddStringToObject(*response, "status", "error");
        cJSON_AddStringToObject(*response, "message",
                                "Cannot delete Default album or album not found");
        return err;
    }

    *response = cJSON_CreateObject();
    cJSON_AddStringToObject(*response, "status", "success");
    return ESP_OK;
}

// Set album enabled/disabled
esp_err_t api_set_album_enabled(const char *name, bool enabled, cJSON **response)
{
    esp_err_t err = album_manager_set_album_enabled(name, enabled);
    if (err != ESP_OK) {
        return err;
    }

    *response = cJSON_CreateObject();
    cJSON_AddStringToObject(*response, "status", "success");
    return ESP_OK;
}

// Display image
esp_err_t api_display_image(const char *filename, cJSON **response)
{
    // Check if display is busy
    if (display_manager_is_busy()) {
        *response = cJSON_CreateObject();
        cJSON_AddStringToObject(*response, "status", "busy");
        cJSON_AddStringToObject(*response, "message", "Display is currently updating");
        return ESP_FAIL;
    }

    // Build absolute path
    char filepath[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, filename);

    esp_err_t err = display_manager_show_image(filepath);
    if (err != ESP_OK) {
        *response = cJSON_CreateObject();
        cJSON_AddStringToObject(*response, "status", "error");
        cJSON_AddStringToObject(*response, "message", "Image not found or display error");
        return err;
    }

    *response = cJSON_CreateObject();
    cJSON_AddStringToObject(*response, "status", "success");
    return ESP_OK;
}

// Delete image
esp_err_t api_delete_image(const char *filename, cJSON **response)
{
    // Build paths
    char filepath[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(filepath, sizeof(filepath), "%s/%s", IMAGE_DIRECTORY, filename);

    // Delete BMP
    if (unlink(filepath) != 0) {
        *response = cJSON_CreateObject();
        cJSON_AddStringToObject(*response, "status", "error");
        cJSON_AddStringToObject(*response, "message", "Failed to delete image");
        return ESP_FAIL;
    }

    // Delete corresponding JPEG thumbnail
    char jpg_filename[TELEGRAM_MAX_FILENAME_LENGTH];
    strncpy(jpg_filename, filename, sizeof(jpg_filename) - 1);
    char *ext = strrchr(jpg_filename, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) {
        strcpy(ext, ".jpg");  // snprintf(ext, 5, "%s", ".jpg");
        char jpg_path[TELEGRAM_MAX_PATH_LENGTH];
        snprintf(jpg_path, sizeof(jpg_path), "%s/%s", IMAGE_DIRECTORY, jpg_filename);
        unlink(jpg_path);  // Ignore errors
    }

    *response = cJSON_CreateObject();
    cJSON_AddStringToObject(*response, "status", "success");
    return ESP_OK;
}

// Get configuration
esp_err_t api_get_config(cJSON **response)
{
    int rotate_interval = display_manager_get_rotate_interval();
    bool auto_rotate = display_manager_get_auto_rotate();
    bool deep_sleep = power_manager_get_deep_sleep_enabled();

    *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(*response, "rotate_interval", rotate_interval);
    cJSON_AddBoolToObject(*response, "auto_rotate", auto_rotate);
    cJSON_AddBoolToObject(*response, "deep_sleep_enabled", deep_sleep);

    // Note: brightness_fstop and contrast are in processing_settings,
    // not in display_manager, so we return default values here
    // You may need to add API to processing_settings to get current values
    cJSON_AddNumberToObject(*response, "brightness_fstop", 0.3);
    cJSON_AddNumberToObject(*response, "contrast", 1.3);

    return ESP_OK;
}

// Update configuration
esp_err_t api_update_config(cJSON *config, cJSON **response)
{
    cJSON *interval_obj = cJSON_GetObjectItem(config, "rotate_interval");
    if (interval_obj && cJSON_IsNumber(interval_obj)) {
        display_manager_set_rotate_interval(interval_obj->valueint);
    }

    cJSON *auto_rotate_obj = cJSON_GetObjectItem(config, "auto_rotate");
    if (auto_rotate_obj && cJSON_IsBool(auto_rotate_obj)) {
        display_manager_set_auto_rotate(cJSON_IsTrue(auto_rotate_obj));
    }

    cJSON *deep_sleep_obj = cJSON_GetObjectItem(config, "deep_sleep_enabled");
    if (deep_sleep_obj && cJSON_IsBool(deep_sleep_obj)) {
        power_manager_set_deep_sleep_enabled(cJSON_IsTrue(deep_sleep_obj));
    }

    // Note: brightness_fstop and contrast updates would require
    // processing_settings API additions

    *response = cJSON_CreateObject();
    cJSON_AddStringToObject(*response, "status", "success");
    return ESP_OK;
}

// Get battery status
esp_err_t api_get_battery(cJSON **response)
{
    int battery_voltage = axp_get_battery_voltage();
    int battery_percent = axp_get_battery_percent();
    bool is_charging = axp_is_charging();
    bool usb_connected = axp_is_usb_connected();
    bool battery_connected = axp_is_battery_connected();

    *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(*response, "battery_voltage_mv", battery_voltage);
    cJSON_AddNumberToObject(*response, "battery_percent", battery_percent);
    cJSON_AddBoolToObject(*response, "charging", is_charging);
    cJSON_AddBoolToObject(*response, "usb_connected", usb_connected);
    cJSON_AddBoolToObject(*response, "battery_connected", battery_connected);

    return ESP_OK;
}

// ESP_LOGI(TAG, "Battery: %d%%, USB: %s", axp_get_battery_percent(), axp_is_usb_connected() ? "YES"
// : "NO");
/*
# 1️ get battery level
curl http://photoframe.local/api/battery/status

# 2️ rotate image (trigger Display-Update)
curl -X POST http://photoframe.local/api/display/rotate

# 3️ display selected image
curl -X POST http://photoframe.local/api/display/show \
  -H "Content-Type: application/json" \
  -d '{"image_path":"/sdcard/images/Default/test.bmp"}'

*/