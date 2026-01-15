// new file for v1.9.0_tlg
#include "telegram_bot.h"

#include <dirent.h>  // for DIR, opendir, readdir
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>  // for struct stat (if needed)
#include <sys/stat.h>
#include <time.h>

#include "album_manager.h"
#include "api_handlers.h"
#include "axp_prot.h"
#include "cJSON.h"
#include "config.h"
#include "display_manager.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"  //pub IP
#include "image_processor.h"
#include "lwip/ip4_addr.h"  //pub IP
#include "nvs.h"
#include "nvs_flash.h"
#include "power_manager.h"

static const char *TAG = "telegram_bot";

#define NVS_TELEGRAM_TOKEN_KEY "tg_token"
#define NVS_TELEGRAM_UPDATEID_KEY "tg_update_id"
#define MAX_HTTP_OUTPUT_BUFFER 8192
#define MAX_DOWNLOAD_SIZE (2 * 1024 * 1024)  // 2 MB max for JPEG download
// see also image_processor.c for const size_t MAX_DECODED_SIZE = 4 * 1024 * 1024;
// ESP32-S3 JPEG-Decoder Limit 10 MB Maximum: ~6400×3840 @ 1:8 scale = 800×480

/*
[Component 			- Capacity 	- System_overhead 	- Available]
PSRAM Total 		- 8 MB 		- ~500 KB			- ~7.5 MB
E paper buffer 		- 120 KB	- permanent			- always occupied
WiFi/stack/tasks	- ~200 KB	- runtime			- always occupied
Available for images-			-					- ~7.2 MB


Download Buffer:    2 MB (PSRAM) - at same time!
Decode Buffer:      4 MB (PSRAM) - at same time!
Resize Buffer:      1.15 MB (800×480×3)
E-Paper Buffer:     120 KB (permanent)
WiFi/System:        200 KB (permanent)
─────────────────────────────────────────────
PEAK:              ~6.5 MB ✅ get in 7.2 MB
*/

// ============= SELECT SAVING MODE ==================
// changed to optimized RAM-only mode
// static bool use_ram_processing = true;  // true = RAM mode + SD-Fallback, false = SD only
// ===================================================

/*
============= LOGIC ==================
download_to_ram() → RAM
  ↓
detect_jpeg_dimensions(buffer)
  ↓
is_portrait?
  ├─ NO  → convert_buffer_to_bmp()
  └─ YES → convert_buffer_to_bmp_with_portrait_combine()
           ├─ Save to portrait/
           ├─ Find pair?
           │   ├─ NO  → Done (wait for next)
           │   └─ YES → combine_portrait_bmps()
           └─ Delete both portraits
  ↓
free(ram_buffer) ✅
=====================================
download_to_ram() → FAIL ❌
  ↓
download_and_save_telegram_file() → SD
  ↓
convert_jpg_to_bmp_with_portrait_combine(file_path)
  ↓ (fread/fwrite)
unlink(jpg_path)
=====================================
*/

static char bot_token[TELEGRAM_TOKEN_MAX_LEN] = {0};
// static char bot_token[TELEGRAM_TOKEN_MAX_LEN] = "ABCD...:06541165...";
static int32_t last_update_id = 0;

// Dynamic HTTP response buffer
static char *http_response_buffer = NULL;
static int http_response_len = 0;
static int http_response_capacity = 0;

extern bool telegram_polling_active;
static bool sleep_requested = false;

static int64_t last_chat_id = 0;
static int64_t configured_chat_id = 0;  // Permanently configured chat ID -123456789

static esp_err_t process_document_as_photo(cJSON *message);

// Configurable Telegram options for Telegram Update + Notification on Auto Rotate Wakeup
static bool notify_on_display_update = true;        // consider dependency to notify_on_timer_wakeup
static bool check_telegram_on_timer_wakeup = true;  // Default enabled, consider dependency
static bool notify_on_timer_wakeup = true;          // consider dependency to notify_on_timer_wakeup
static bool notify_on_sleep = false;                // consider dependency to notify_on_timer_wakeup
/*
                bool check_on_timer = telegram_bot_get_check_on_timer_wakeup();
                bool notify_on_timer = telegram_bot_get_notify_on_timer_wakeup();
                bool notify_on_sleep = telegram_bot_get_notify_on_sleep();
                bool notify_on_display = telegram_bot_get_notify_on_display_update();
*/

/**
 * @brief Check if a photo send operation is in progress
 * @return true if photo is being sent, false otherwise
 */
// Tracking for photo send operations
// Flag to wait for telegram bevore WiFi disconnect
static volatile bool photo_send_in_progress = false;
static SemaphoreHandle_t photo_send_mutex = NULL;

/**
 * @brief Check if caption contains display trigger keyword
 * @param caption Caption text to check
 * @return true if should display immediately
 */
static bool should_display_immediately(const char *caption)
{
    if (!caption || strlen(caption) == 0) {
        return false;
    }

    // Check for trigger keywords (case-insensitive)
    char lower_caption[TELEGRAM_MAX_CAPTION_LENGTH];
    strncpy(lower_caption, caption, sizeof(lower_caption) - 1);
    lower_caption[sizeof(lower_caption) - 1] = '\0';

    // Convert to lowercase
    for (int i = 0; lower_caption[i]; i++) {
        lower_caption[i] = tolower((unsigned char) lower_caption[i]);
    }

    // Check for keywords
    if (strstr(lower_caption, "#show") || strstr(lower_caption, "#display") ||
        strstr(lower_caption, "#now") || strstr(lower_caption, "show now") ||
        strstr(lower_caption, "display now")) {
        return true;
    }

    return false;
}

/**
 * @brief Remove display trigger keywords from caption text
 * @param caption Original caption text
 * @param output Output buffer for filtered text
 * @param output_size Size of output buffer
 */
static void remove_display_triggers(const char *caption, char *output, size_t output_size)
{
    if (!caption || !output || output_size == 0) {
        return;
    }

    // Copy caption to output
    strncpy(output, caption, output_size - 1);
    output[output_size - 1] = '\0';

    // Create lowercase copy for case-insensitive search
    char lower[TELEGRAM_MAX_CAPTION_LENGTH];
    strncpy(lower, output, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';

    for (int i = 0; lower[i]; i++) {
        lower[i] = tolower((unsigned char) lower[i]);
    }

    // List of triggers to remove
    const char *triggers[] = {"#show", "#display", "#now", "show now", "display now"};
    const int trigger_count = 5;

    // Remove each trigger
    for (int i = 0; i < trigger_count; i++) {
        char *pos;
        while ((pos = strstr(lower, triggers[i])) != NULL) {
            int offset = pos - lower;
            int trigger_len = strlen(triggers[i]);

            // Remove from both strings
            memmove(output + offset, output + offset + trigger_len,
                    strlen(output + offset + trigger_len) + 1);
            memmove(lower + offset, lower + offset + trigger_len,
                    strlen(lower + offset + trigger_len) + 1);
        }
    }

    // Trim whitespace
    char *start = output;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) {
        start++;
    }

    if (start != output) {
        memmove(output, start, strlen(start) + 1);
    }

    size_t len = strlen(output);
    while (len > 0 && (output[len - 1] == ' ' || output[len - 1] == '\t' ||
                       output[len - 1] == '\n' || output[len - 1] == '\r')) {
        output[--len] = '\0';
    }
}

// Helper to get random image
static esp_err_t get_next_random_image(char *image_path, size_t path_size)
{
    // Get enabled albums
    char **enabled_albums = NULL;
    int album_count = 0;

    if (album_manager_get_enabled_albums(&enabled_albums, &album_count) != ESP_OK ||
        album_count == 0) {
        ESP_LOGW(TAG, "No enabled albums");
        return ESP_FAIL;
    }

    // Count total images
    int total_count = 0;
    for (int i = 0; i < album_count; i++) {
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));
        DIR *dir = opendir(album_path);
        if (!dir)
            continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                    total_count++;
                }
            }
        }
        closedir(dir);
    }

    if (total_count == 0) {
        album_manager_free_album_list(enabled_albums, album_count);
        return ESP_FAIL;
    }

    // Build list and select random
    char **image_list = malloc(total_count * sizeof(char *));
    int idx = 0;

    for (int i = 0; i < album_count; i++) {
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));
        DIR *dir = opendir(album_path);
        if (!dir)
            continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && idx < total_count) {
            if (entry->d_type == DT_REG) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                    image_list[idx] = malloc(512);
                    snprintf(image_list[idx], 512, "%s/%s", album_path, entry->d_name);
                    idx++;
                }
            }
        }
        closedir(dir);
    }

    album_manager_free_album_list(enabled_albums, album_count);

    // Select random
    int random_idx = esp_random() % total_count;
    strncpy(image_path, image_list[random_idx], path_size - 1);
    image_path[path_size - 1] = '\0';

    // Free list
    for (int i = 0; i < total_count; i++) {
        free(image_list[i]);
    }
    free(image_list);

    return ESP_OK;
}

// Load settings from NVS
static void telegram_bot_load_settings(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t val = 0;

        // Load check_on_timer setting (default: disabled)
        if (nvs_get_u8(nvs_handle, NVS_TELEGRAM_CHECK_ON_TIMER_KEY, &val) == ESP_OK) {
            check_telegram_on_timer_wakeup = (val != 0);
        }

        // Load notify_on_timer setting (default: disabled)
        val = 0;
        if (nvs_get_u8(nvs_handle, NVS_TELEGRAM_NOTIFY_ON_TIMER_KEY, &val) == ESP_OK) {
            notify_on_timer_wakeup = (val != 0);
        }

        // Load notify_on_sleep setting (default: enabled)
        val = 1;  // Default ON
        if (nvs_get_u8(nvs_handle, "tg_notify_sleep", &val) == ESP_OK) {
            notify_on_sleep = (val != 0);
        }

        // Load notify_on_display_update setting (default: enabled)
        val = 1;  // Default ON
        if (nvs_get_u8(nvs_handle, "tg_notify_display", &val) == ESP_OK) {
            notify_on_display_update = (val != 0);
        }

        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG,
             "Telegram settings: check_on_timer=%d, notify_on_timer=%d, notify_on_sleep=%d, "
             "notify_on_display=%d",
             check_telegram_on_timer_wakeup, notify_on_timer_wakeup, notify_on_sleep,
             notify_on_display_update);
    // ESP_LOGI(TAG, "Telegram settings: check_on_timer=%d, notify_on_timer=%d, notify_on_sleep=%d",
    // check_telegram_on_timer_wakeup, notify_on_timer_wakeup, notify_on_sleep);
    // ESP_LOGI(TAG, "Telegram settings: check_on_timer=%d, notify_on_timer=%d",
    // check_telegram_on_timer_wakeup, notify_on_timer_wakeup);
}

// Getter/Setter
bool telegram_bot_get_notify_on_display_update(void)
{
    return notify_on_display_update;
}

esp_err_t telegram_bot_set_notify_on_display_update(bool enabled)
{
    notify_on_display_update = enabled;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs_handle, "tg_notify_display", enabled ? 1 : 0);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Telegram notify on display update: %s", enabled ? "enabled" : "disabled");
    return err;
}

// Get check on timer wakeup setting
bool telegram_bot_get_check_on_timer_wakeup(void)
{
    return check_telegram_on_timer_wakeup;
}

// Set check on timer wakeup setting
esp_err_t telegram_bot_set_check_on_timer_wakeup(bool enabled)
{
    check_telegram_on_timer_wakeup = enabled;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_TELEGRAM_CHECK_ON_TIMER_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Telegram check on timer wakeup: %s", enabled ? "enabled" : "disabled");
    return err;
}

// Get notify on timer wakeup setting
bool telegram_bot_get_notify_on_timer_wakeup(void)
{
    return notify_on_timer_wakeup;
}

// Set notify on timer wakeup setting
esp_err_t telegram_bot_set_notify_on_timer_wakeup(bool enabled)
{
    notify_on_timer_wakeup = enabled;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_TELEGRAM_NOTIFY_ON_TIMER_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Telegram notify on timer wakeup: %s", enabled ? "enabled" : "disabled");
    return err;
}

// Send wake-up notification with battery status
esp_err_t telegram_bot_notify_wakeup(int64_t chat_id)
{
    ESP_LOGI(TAG, "Sending wake-up notification");

    // Get battery status
    cJSON *battery_json = NULL;
    esp_err_t err = api_get_battery(&battery_json);

    char message[TELEGRAM_MAX_MESSAGE_LENGTH_S];

    if (err == ESP_OK && battery_json) {
        int voltage = cJSON_GetObjectItem(battery_json, "battery_voltage_mv")->valueint;
        int percent = cJSON_GetObjectItem(battery_json, "battery_percent")->valueint;
        bool charging = cJSON_IsTrue(cJSON_GetObjectItem(battery_json, "charging"));
        bool usb = cJSON_IsTrue(cJSON_GetObjectItem(battery_json, "usb_connected"));

        snprintf(message, sizeof(message),
                 "⏰ <b>Wake Up</b>\n\n"
                 "🔋 <b>Battery Status</b>\n"
                 "Level: %d%% (%d mV)\n"
                 "Charging: %s\n"
                 "USB: %s",
                 percent, voltage, charging ? "Yes ⚡" : "No",
                 usb ? "Connected 🔌" : "Disconnected");

        cJSON_Delete(battery_json);
    } else {
        // Fallback if battery status fails
        snprintf(message, sizeof(message),
                 "⏰ <b>Wake Up</b>\n\n"
                 "Device has woken up from sleep.");
    }

    err = telegram_bot_send_message(chat_id, message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send wake-up notification");
        return err;
    }

    ESP_LOGI(TAG, "Wake-up notification sent successfully");
    return ESP_OK;
}

// Getter/Setter
bool telegram_bot_get_notify_on_sleep(void)
{
    return notify_on_sleep;
}

esp_err_t telegram_bot_set_notify_on_sleep(bool enabled)
{
    notify_on_sleep = enabled;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(nvs_handle, "tg_notify_sleep", enabled ? 1 : 0);
    if (err == ESP_OK) {
        nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Telegram notify on sleep: %s", enabled ? "enabled" : "disabled");
    return err;
}

// Send sleep notification with current configuration before entering sleep
esp_err_t telegram_bot_notify_sleep(int64_t chat_id)
{
    ESP_LOGI(TAG, "Sending sleep notification");

    // Get current configuration
    cJSON *config_json = NULL;
    esp_err_t err = api_get_config(&config_json);

    char message[TELEGRAM_MAX_MESSAGE_LENGTH_M];

    if (err == ESP_OK && config_json) {
        // Parse config
        int rotate_interval = cJSON_GetObjectItem(config_json, "rotate_interval")->valueint;
        bool auto_rotate = cJSON_IsTrue(cJSON_GetObjectItem(config_json, "auto_rotate"));
        bool deep_sleep_enabled =
            cJSON_IsTrue(cJSON_GetObjectItem(config_json, "deep_sleep_enabled"));

        // Get battery info
        cJSON *battery_json = NULL;
        char battery_info[TELEGRAM_MAX_LINE_MESSAGE_LENGTH] = "";

        if (api_get_battery(&battery_json) == ESP_OK) {
            int battery_percent = cJSON_GetObjectItem(battery_json, "battery_percent")->valueint;
            bool charging = cJSON_IsTrue(cJSON_GetObjectItem(battery_json, "charging"));
            bool usb_connected = cJSON_IsTrue(cJSON_GetObjectItem(battery_json, "usb_connected"));

            if (charging) {
                snprintf(battery_info, sizeof(battery_info), "⚡ Charging - %d%%", battery_percent);
            } else if (usb_connected) {
                snprintf(battery_info, sizeof(battery_info), "🔌 USB Connected - %d%%",
                         battery_percent);
            } else {
                snprintf(battery_info, sizeof(battery_info), "🔋 On Battery - %d%%",
                         battery_percent);
            }

            cJSON_Delete(battery_json);
        } else {
            strcpy(battery_info, "Battery status unavailable");
        }

        // Build message
        snprintf(message, sizeof(message),
                 "😴 <b>Going to Sleep</b>\n\n"
                 "⚙️ <b>Configuration</b>\n"
                 "• Auto-Rotate: %s\n"
                 "• Rotation Interval: %d seconds\n"
                 "• Deep Sleep: %s\n\n"
                 "🔋 <b>Battery</b>\n"
                 "%s\n\n"
                 "💤 Entering sleep mode now...",
                 auto_rotate ? "✅ Enabled" : "❌ Disabled", rotate_interval,
                 deep_sleep_enabled ? "✅ Enabled" : "❌ Disabled", battery_info);

        cJSON_Delete(config_json);
    } else {
        // Fallback if config fails
        snprintf(message, sizeof(message),
                 "😴 <b>Going to Sleep</b>\n\n"
                 "Device is entering deep sleep mode.");
    }

    err = telegram_bot_send_message(chat_id, message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send sleep notification");
        return err;
    }

    ESP_LOGI(TAG, "Sleep notification sent successfully");
    return ESP_OK;
}

// Photo Upload
// Structure for Task Parameter
typedef struct {
    int64_t chat_id;
    char photo_path[TELEGRAM_MAX_PATH_LENGTH];
    char caption[TELEGRAM_MAX_CAPTION_LENGTH];
} photo_send_params_t;


// HTTP event handler for JSON responses
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!http_response_buffer) {
            http_response_capacity = MAX_HTTP_OUTPUT_BUFFER;
            http_response_buffer = malloc(http_response_capacity);
            if (!http_response_buffer) {
                ESP_LOGE(TAG, "Failed to allocate response buffer");
                return ESP_FAIL;
            }
        }

        if (http_response_len + evt->data_len < http_response_capacity) {
            memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
            http_response_len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "HTTP response buffer overflow");
        }
        break;
    // Status-Logging
    case HTTP_EVENT_ON_FINISH:
        if (http_response_buffer && http_response_len > 0) {
            http_response_buffer[http_response_len] = '\0';
            ESP_LOGI(TAG, "HTTP response received: %d bytes", http_response_len);
            // Log bei Fehler
            if (strstr((char *) http_response_buffer, "\"ok\":false")) {
                ESP_LOGE(TAG, "Error response: %s", http_response_buffer);
            }
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

esp_err_t telegram_bot_init(void)
{
    ESP_LOGI(TAG, "=== Telegram bot init START ===");

    // Init Photo-Send-Mutex
    if (!photo_send_mutex) {
        photo_send_mutex = xSemaphoreCreateMutex();
        if (!photo_send_mutex) {
            ESP_LOGE(TAG, "Failed to create photo send mutex");
            return ESP_FAIL;
        }
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    ESP_LOGI(TAG, "NVS open result: %s", esp_err_to_name(err));

    if (err == ESP_OK) {
        size_t token_len = sizeof(bot_token);
        nvs_get_str(nvs_handle, NVS_TELEGRAM_TOKEN_KEY, bot_token, &token_len);
        nvs_get_i32(nvs_handle, NVS_TELEGRAM_UPDATEID_KEY, &last_update_id);

        // Load configured chat ID
        int64_t chat_id_temp = 0;
        if (nvs_get_i64(nvs_handle, NVS_TELEGRAM_CHAT_ID_KEY, &chat_id_temp) == ESP_OK) {
            configured_chat_id = chat_id_temp;
            last_chat_id = chat_id_temp;  // Also set last_chat_id
            ESP_LOGI(TAG, "Loaded configured chat ID: %" PRId64, configured_chat_id);
        }

        nvs_close(nvs_handle);
    }

    if (strlen(bot_token) > 0) {
        ESP_LOGI(TAG, "Telegram bot initialized with token (last update ID: %" PRId32 ")",
                 last_update_id);
    } else {
        ESP_LOGI(TAG, "Telegram bot initialized without token");
    }

    // Hardcoded Chat ID (temporary, will be replaced by web interface)
    if (configured_chat_id == 0) {
        // SET YOUR CHAT ID HERE:
        configured_chat_id = 0;  // <-- YOUR CHAT ID -123456789
        last_chat_id = configured_chat_id;
        ESP_LOGI(TAG, "Using hardcoded chat ID: %" PRId64, configured_chat_id);

        // Optional: Save to NVS
        telegram_bot_set_chat_id(configured_chat_id);
    }

    // Load settings
    telegram_bot_load_settings();

    ESP_LOGI(TAG, "=== Telegram bot init DONE ===");
    return ESP_OK;
}

// Set Chat ID
esp_err_t telegram_bot_set_chat_id(int64_t chat_id)
{
    if (chat_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    configured_chat_id = chat_id;
    last_chat_id = chat_id;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_i64(nvs_handle, NVS_TELEGRAM_CHAT_ID_KEY, chat_id);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram chat ID set: %" PRId64, chat_id);
    return ESP_OK;
}

// Get Chat ID
esp_err_t telegram_bot_get_chat_id(int64_t *chat_id)
{
    if (!chat_id) {
        return ESP_ERR_INVALID_ARG;
    }

    // Priority: configured_chat_id > last_chat_id
    if (configured_chat_id != 0) {
        *chat_id = configured_chat_id;
    } else if (last_chat_id != 0) {
        *chat_id = last_chat_id;
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

// Has Chat ID
bool telegram_bot_has_chat_id(void)
{
    return (configured_chat_id != 0 || last_chat_id != 0);
}

// Update telegram_bot_get_last_chat_id():
int64_t telegram_bot_get_last_chat_id(void)
{
    // Return configured_chat_id if set, otherwise last_chat_id
    return (configured_chat_id != 0) ? configured_chat_id : last_chat_id;
}

esp_err_t telegram_bot_set_token(const char *token)
{
    if (!token || strlen(token) == 0 || strlen(token) >= TELEGRAM_TOKEN_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(bot_token, token, TELEGRAM_TOKEN_MAX_LEN - 1);
    bot_token[TELEGRAM_TOKEN_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_TELEGRAM_TOKEN_KEY, bot_token);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram bot token set");
    return ESP_OK;
}

esp_err_t telegram_bot_get_token(char *token, size_t len)
{
    if (!token || len < strlen(bot_token) + 1) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(token, bot_token, len);
    return ESP_OK;
}

bool telegram_bot_has_token(void)
{
    return (strlen(bot_token) > 0);
}

// Function to send messages:
// Send message to Telegram chat (with Markdown support)
esp_err_t telegram_bot_send_message(int64_t chat_id, const char *text)
{
    char url[TELEGRAM_MAX_URL_LENGTH];
    snprintf(url, sizeof(url), "https://%s/bot%s/sendMessage", TELEGRAM_API_BASE, bot_token);

    // Create JSON body
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "chat_id", (double) chat_id);
    cJSON_AddStringToObject(json, "text", text);
    cJSON_AddStringToObject(json, "parse_mode", "HTML");  // HTML no markdown

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!json_str) {
        ESP_LOGE(TAG, "Failed to create JSON");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending message to chat %" PRId64, chat_id);

    // Reset response buffer
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    http_response_len = 0;
    http_response_capacity = 0;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TELEGRAM_SHORT_TIMEOUT_MS,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    // Int Client
    // esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // 2. ⚠️ ToDo: Check if this needed!
    // const char *boundary = "----TelegramBotBoundary";
    // char content_type[128];
    // snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    //---------------------- Set Header
    // esp_err_t err = esp_http_client_set_header(client, "Content-Type", content_type);
    // if (err != ESP_OK) {
    //	ESP_LOGE(TAG, "Failed to set Content-Type header: %s", esp_err_to_name(err));
    //	esp_http_client_cleanup(client);
    //	free(complete_body);
    //	free(url);
    //	return err;
    //}
    //--------------------- Set POST Body
    // err = esp_http_client_set_post_field(client, (const char*)complete_body, total_len);
    // if (err != ESP_OK) {
    //	ESP_LOGE(TAG, "Failed to set POST field: %s", esp_err_to_name(err));
    //	esp_http_client_cleanup(client);
    //	free(complete_body);
    //	free(url);
    //	return err;
    //}

    // Set Header
    // esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set header: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    // Set POST Body
    // esp_http_client_set_post_field(client, json_str, strlen(json_str));
    err = esp_http_client_set_post_field(client, json_str, strlen(json_str));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set POST field: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(json_str);
        return err;
    }

    // Perform Request
    // esp_err_t err = esp_http_client_perform(client);
    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        // return err;
        return false;  // Bei Fehler false zurückgeben, nicht true!
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        if (http_response_buffer && http_response_len > 0) {
            http_response_buffer[http_response_len] = '\0';
            ESP_LOGE(TAG, "Telegram API error response: %s", http_response_buffer);
        }
        ESP_LOGE(TAG, "Failed to send message: %s (status %d)", esp_err_to_name(err), status);
        free(json_str);
        return ESP_FAIL;
    }

    free(json_str);
    ESP_LOGI(TAG, "Message sent successfully");
    return ESP_OK;
}

esp_err_t telegram_bot_send_photo(int64_t chat_id, const char *photo_path, const char *caption)
{
    ESP_LOGI(TAG, "=== SEND PHOTO START ===");

    // Memory-Check
    ESP_LOGI(TAG, "Free internal RAM: %lu bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "Largest internal block: %lu bytes",
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    ESP_LOGI(TAG, "Sending photo to chat %" PRId64 ": %s", chat_id, photo_path);

    // Check if file exists
    struct stat st;
    if (stat(photo_path, &st) != 0) {
        ESP_LOGE(TAG, "Photo file not found: %s", photo_path);
        return ESP_FAIL;
    }

    long file_size = st.st_size;
    ESP_LOGI(TAG, "Photo file size: %ld bytes", file_size);

    if (file_size > MAX_DOWNLOAD_SIZE || file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    // Read file into SPIRAM buffer
    FILE *fp = fopen(photo_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open photo: %s", photo_path);
        return ESP_FAIL;
    }

    uint8_t *file_buffer = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buffer) {
        ESP_LOGE(TAG, "Failed to allocate file buffer");
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(file_buffer, 1, file_size, fp);
    fclose(fp);

    if (read_bytes != file_size) {
        ESP_LOGE(TAG, "Failed to read file completely");
        free(file_buffer);
        return ESP_FAIL;
    }

    // Prepare URL
    char *url = heap_caps_malloc(512, MALLOC_CAP_8BIT);
    if (!url) {
        free(file_buffer);
        return ESP_ERR_NO_MEM;
    }
    snprintf(url, 512, "https://%s/bot%s/sendPhoto", TELEGRAM_API_BASE, bot_token);

    // Boundary
    const char *boundary = "----TelegramBotBoundary";

    // Build multipart body in SPIRAM
    char *body_header = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!body_header) {
        free(file_buffer);
        free(url);
        return ESP_ERR_NO_MEM;
    }

    // Format: Multipart with chat_id, photo, and optional caption
    int header_len;
    if (caption && strlen(caption) > 0) {
        // With caption
        header_len =
            snprintf(body_header, 2048,
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"chat_id\"\r\n"
                     "\r\n"
                     "%" PRId64
                     "\r\n"
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"caption\"\r\n"
                     "\r\n"
                     "%s\r\n"
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "\r\n",
                     boundary, chat_id, boundary, caption, boundary);
    } else {
        // Without caption
        header_len =
            snprintf(body_header, 2048,
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"chat_id\"\r\n"
                     "\r\n"
                     "%" PRId64
                     "\r\n"
                     "--%s\r\n"
                     "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "\r\n",
                     boundary, chat_id, boundary);
    }

    // Static footer - guaranteed correct format
    const char *footer_str = "\r\n------TelegramBotBoundary--\r\n";
    int footer_len = strlen(footer_str);

    ESP_LOGI(TAG, "Multipart sizes - header: %d, file: %ld, footer: %d bytes", header_len,
             file_size, footer_len);

    // Calculate total length
    int total_len = header_len + file_size + footer_len;
    ESP_LOGI(TAG, "Total body length: %d bytes", total_len);

    // Allocate complete body in SPIRAM
    uint8_t *complete_body = heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM);
    if (!complete_body) {
        ESP_LOGE(TAG, "Failed to allocate complete body (%d bytes)", total_len);
        free(file_buffer);
        free(url);
        free(body_header);
        return ESP_ERR_NO_MEM;
    }

    // Assemble body
    memcpy(complete_body, body_header, header_len);
    memcpy(complete_body + header_len, file_buffer, file_size);
    memcpy(complete_body + header_len + file_size, footer_str, footer_len);

    // Debug: Print last 50 bytes (shows footer)
    ESP_LOGI(TAG, "Last 50 bytes of body:");
    ESP_LOG_BUFFER_HEXDUMP(TAG, complete_body + total_len - 50, 50, ESP_LOG_INFO);

    // Free intermediate buffers
    free(file_buffer);
    free(body_header);

    ESP_LOGI(TAG, "Sending HTTP POST to Telegram...");

    /*
// Build multipart body in SPIRAM
char *body_header = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
if (!body_header) {
    free(file_buffer);
    free(url);
    return ESP_ERR_NO_MEM;
}

int header_len = snprintf(body_header, 2048,
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
    "%" PRId64 "\r\n"
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
    "%s\r\n"
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
    "Content-Type: image/jpeg\r\n\r\n",
    boundary, chat_id, boundary, caption ? caption : "", boundary);

char *body_footer = heap_caps_malloc(256, MALLOC_CAP_8BIT);
if (!body_footer) {
    free(file_buffer);
    free(url);
    free(body_header);
    return ESP_ERR_NO_MEM;
}
int footer_len = snprintf(body_footer, 256, "\r\n--%s--\r\n", boundary);

// Calculate total length
int total_len = header_len + file_size + footer_len;
ESP_LOGI(TAG, "Total body length: %d bytes", total_len);

// Allocate complete body in SPIRAM
uint8_t *complete_body = heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM);
if (!complete_body) {
    ESP_LOGE(TAG, "Failed to allocate complete body (%d bytes)", total_len);
    free(file_buffer);
    free(url);
    free(body_header);
    free(body_footer);
    return ESP_ERR_NO_MEM;
}

// Assemble body
memcpy(complete_body, body_header, header_len);
memcpy(complete_body + header_len, file_buffer, file_size);
memcpy(complete_body + header_len + file_size, body_footer, footer_len);

// Free intermediate buffers
free(file_buffer);
free(body_header);
free(body_footer);

ESP_LOGI(TAG, "Sending HTTP POST to Telegram...");

    */

    // HTTP client config

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = TELEGRAM_MAX_DOWN_JPG_SIZE,
        .buffer_size_tx = TELEGRAM_MAX_DOWN_JPG_SIZE,
    };

    /*
    // with PSRAM as Buffer
    esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
            .buffer_size = TELEGRAM_MAX_DOWN_JPG_SIZE,
            .buffer_size_tx = TELEGRAM_MAX_DOWN_JPG_SIZE,
            .cert_pem = telegram_server_cert_pem_start,
            .use_global_ca_store = false,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .is_async = false,
            .skip_cert_common_name_check = false,
            .buffer_cap = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
    };
    */

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    // Set headers
    char *content_type = heap_caps_malloc(128, MALLOC_CAP_8BIT);
    if (!content_type) {
        ESP_LOGE(TAG, "Failed to allocate content_type header");
        free(complete_body);
        free(url);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    snprintf(content_type, 128, "multipart/form-data; boundary=%s", boundary);
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", content_type);
    // ESP_LOGI(TAG, "Setting Content-Type header: %s", content_type);
    // esp_http_client_set_header(client, "Content-Type", content_type);
    // esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set HTTP header: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(complete_body);
        free(url);
        free(content_type);
        return err;
    }

    ESP_LOGI(TAG, "Setting POST field (%d bytes)...", total_len);
    err = esp_http_client_set_post_field(client, (const char *) complete_body, total_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set POST field: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(complete_body);
        free(url);
        free(content_type);
        return err;
    }
    ESP_LOGI(TAG, "POST field set successfully");
    // esp_http_client_set_post_field(client, (const char*)complete_body, total_len);

    // Perform request
    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    // Event Handler hat Response bereits gepuffert!
    /*
    if (status != 200 && http_response_buffer && http_response_len > 0) {
            http_response_buffer[http_response_len] = '\0';
            ESP_LOGE(TAG, "=== Telegram API Response ===");
            ESP_LOGE(TAG, "%s", (char*)http_response_buffer);

            // JSON parsen...
            cJSON *error_json = cJSON_Parse((char*)http_response_buffer);
            // ... (wie oben)
    }
    */
    ESP_LOGI(TAG, "HTTP perform result: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "HTTP status code: %d", status);

    // Memory status
    ESP_LOGI(TAG, "Memory after HTTP request - Free heap: %lu bytes, SPIRAM: %lu bytes",
             esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Response-Body bei Fehler lesen (MUSS VOR cleanup() passieren!)
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP request failed!");

        int content_length = esp_http_client_get_content_length(client);
        ESP_LOGW(TAG, "Content-Length: %d", content_length);

        // Response Body lesen
        if (content_length > 0 && content_length < 512) {
            char error_buf[512] = {0};
            int total_read = 0;
            int read_len;

            while (total_read < content_length && total_read < 511) {
                read_len = esp_http_client_read(client, error_buf + total_read,
                                                content_length - total_read);
                if (read_len <= 0) {
                    break;
                }
                total_read += read_len;
            }

            if (total_read > 0) {
                error_buf[total_read] = '\0';
                ESP_LOGE(TAG, "=== Telegram API Response ===");
                ESP_LOGE(TAG, "%s", error_buf);

                // JSON parsen
                cJSON *error_json = cJSON_Parse(error_buf);
                if (error_json) {
                    cJSON *desc = cJSON_GetObjectItem(error_json, "description");
                    cJSON *error_code_obj = cJSON_GetObjectItem(error_json, "error_code");

                    if (error_code_obj && cJSON_IsNumber(error_code_obj)) {
                        ESP_LOGE(TAG, "Telegram error_code: %d", error_code_obj->valueint);
                    }
                    if (desc && cJSON_IsString(desc)) {
                        ESP_LOGE(TAG, "Telegram error: %s", desc->valuestring);
                    }

                    cJSON_Delete(error_json);
                }
            } else {
                ESP_LOGW(TAG, "No data read from response (total_read=%d)", total_read);
            }
        }

        // Log connection info
        char url_buffer[256];
        esp_http_client_get_url(client, url_buffer, sizeof(url_buffer));
        ESP_LOGE(TAG, "Failed URL: %s", url_buffer);
    }
    // ===== END Logging =====

    // ESP_LOGI(TAG, "HTTP response: %s (status %d)", esp_err_to_name(err), status);

    // Cleanup
    esp_http_client_cleanup(client);
    free(complete_body);
    free(url);
    free(content_type);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Failed to send photo: %s (status %d)", esp_err_to_name(err), status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, " Photo sent successfully");
    ESP_LOGI(TAG, "=== SEND PHOTO END ===");
    return ESP_OK;
}

// Send notification when display is updated with image info and thumbnail
esp_err_t telegram_bot_notify_display_update(int64_t chat_id, const char *image_path)
{
    ESP_LOGI(TAG, "=== NOTIFY DISPLAY UPDATE START ===");

    // Check if display update notification is enabled
    if (!notify_on_display_update) {
        ESP_LOGI(TAG, "Display update notification disabled, skipping");
        return ESP_OK;  // Not an error, just skipped
    }

    ESP_LOGI(TAG, "Chat ID: %" PRId64, chat_id);
    ESP_LOGI(TAG, "Notifying display update: %s", image_path);

    // Extract filename from path (e.g., "Default/telegram_102.bmp" -> "telegram_102")
    const char *filename = strrchr(image_path, '/');
    if (filename) {
        filename++;  // Skip the '/'
    } else {
        filename = image_path;
    }
    ESP_LOGI(TAG, "Filename: %s", filename);
    // Remove .bmp extension
    char base_name[TELEGRAM_MAX_FILENAME_LENGTH];
    strncpy(base_name, filename, sizeof(base_name) - 1);
    base_name[sizeof(base_name) - 1] = '\0';
    char *ext = strrchr(base_name, '.');
    if (ext) {
        *ext = '\0';
    }
    ESP_LOGI(TAG, "Base name: %s", base_name);
    // Build message
    char message[TELEGRAM_MAX_MESSAGE_LENGTH_S];
    snprintf(message, sizeof(message),
             "🖼️ <b>Display Updated</b>\n\n"
             "Image: <code>%s</code>",
             filename);

    // Send text message
    ESP_LOGI(TAG, "Sending text message...");
    esp_err_t err = telegram_bot_send_message(chat_id, message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send display update message");
        return err;
    }
    ESP_LOGI(TAG, "Text message sent successfully");

    // Try to find and send thumbnail
    // Build path to thumbnail: /sdcard/images/<album>/<basename>.jpg OR <basename>_thumb.jpg
    // char thumbnail_path[512];

    // Extract album from image_path (e.g., "Default/telegram_102.bmp" -> "Default")
    // char album[128] = "Default";
    char album[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
    strcpy(album, "Default");  // Default fallback

    // Check if image_path contains a slash (has album prefix)
    const char *first_slash = strchr(image_path, '/');
    if (first_slash && first_slash != image_path) {
        // Extract album name before first slash
        size_t album_len = first_slash - image_path;
        if (album_len > 0 && album_len < sizeof(album)) {
            strncpy(album, image_path, album_len);
            album[album_len] = '\0';
            ESP_LOGI(TAG, "Extracted album from path: '%s'", album);
        }
    } else if (strstr(image_path, IMAGE_DIRECTORY) == image_path) {
        // Path is absolute: /sdcard/images/AlbumName/file.bmp
        // Skip IMAGE_DIRECTORY prefix
        const char *after_base = image_path + strlen(IMAGE_DIRECTORY);
        if (*after_base == '/')
            after_base++;  // Skip leading slash

        const char *next_slash = strchr(after_base, '/');
        if (next_slash) {
            size_t album_len = next_slash - after_base;
            if (album_len > 0 && album_len < sizeof(album)) {
                strncpy(album, after_base, album_len);
                album[album_len] = '\0';
                ESP_LOGI(TAG, "Extracted album from absolute path: '%s'", album);
            }
        }
    } else {
        ESP_LOGW(TAG, "No album in path, using default: '%s'", album);
    }

    ESP_LOGI(TAG, "Album: '%s'", album);

    // Build thumbnail path
    char thumbnail_path[TELEGRAM_MAX_PATH_LENGTH];
    ESP_LOGI(TAG, "Looking for thumbnail: %s", thumbnail_path);

    // Try without _thumb.jpg suffix first (for normal images)
    snprintf(thumbnail_path, sizeof(thumbnail_path), "%s/%s/%s.jpg", IMAGE_DIRECTORY, album,
             base_name);

    // Check if thumbnail exists
    struct stat st;
    bool thumb_found = (stat(thumbnail_path, &st) == 0);

    // If not found, try with _thumb suffix (for combined images)
    if (!thumb_found) {
        snprintf(thumbnail_path, sizeof(thumbnail_path), "%s/%s/%s_thumb.jpg", IMAGE_DIRECTORY,
                 album, base_name);
        ESP_LOGI(TAG, "Trying alternative path: %s", thumbnail_path);
        thumb_found = (stat(thumbnail_path, &st) == 0);
    }

    if (thumb_found) {
        ESP_LOGI(TAG, " Thumbnail found! Size: %ld bytes", st.st_size);

        // Setze Flag VOR synchronem Upload
        if (photo_send_mutex && xSemaphoreTake(photo_send_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            photo_send_in_progress = true;
            xSemaphoreGive(photo_send_mutex);
        }

        // Synchron Upload not Async
        ESP_LOGI(TAG, "Sending photo (synchronous, may block 2-3 seconds)...");
        // err = telegram_bot_send_photo_async(chat_id, thumbnail_path, filename);
        err = telegram_bot_send_photo(chat_id, thumbnail_path, filename);

        // delete Flag after Upload
        if (photo_send_mutex && xSemaphoreTake(photo_send_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            photo_send_in_progress = false;
            xSemaphoreGive(photo_send_mutex);
        }

        // ESP_LOGI(TAG, "Sending photo...");
        // err = telegram_bot_send_photo(chat_id, thumbnail_path, filename);
        // err = telegram_bot_send_photo_async(chat_id, thumbnail_path, filename);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "✗ Failed to send thumbnail: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, " Thumbnail sent successfully");
            // ESP_LOGI(TAG, " Thumbnail sent (full photo upload disabled to save memory)");
        }
    } else {
        ESP_LOGW(TAG, "✗ Thumbnail not found: %s (errno: %d)", thumbnail_path, errno);
        telegram_bot_send_message(chat_id, "ℹ️ <i>No thumbnail available</i>");
    }
    ESP_LOGI(TAG, "=== NOTIFY DISPLAY UPDATE END ===");
    return ESP_OK;
}

// Fallback function: Download directly to SD card (used when RAM download fails)
// Marked unused to suppress warning when not used in optimized path
static esp_err_t download_and_save_telegram_file(const char *file_path, char *output_path,
                                                 size_t output_len) __attribute__((unused));

static esp_err_t download_and_save_telegram_file(const char *file_path, char *output_path,
                                                 size_t output_len)
{
    char url[TELEGRAM_MAX_URL_LENGTH];
    snprintf(url, sizeof(url), "https://%s/file/bot%s/%s", TELEGRAM_API_BASE, bot_token, file_path);

    ESP_LOGI(TAG, "Downloading file from Telegram: %s", file_path);
    ESP_LOGI(TAG, "Downloading file from: %s", url);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .buffer_size = TELEGRAM_MAX_DOWN_JPG_SIZE,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    // esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        // free(url);
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // Check size limit
    int content_length = esp_http_client_fetch_headers(client);
    if (content_length > MAX_DOWNLOAD_SIZE) {
        ESP_LOGE(TAG, "File too large: %d bytes (max %d)", content_length, MAX_DOWNLOAD_SIZE);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Content-Length: %d bytes", content_length);

    // Allocate buffer in SPIRAM
    uint8_t *buffer = heap_caps_malloc(content_length > 0 ? content_length : MAX_DOWNLOAD_SIZE,
                                       MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate download buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    // Download to RAM
    int total_read = 0;
    int buffer_capacity = content_length > 0 ? content_length : MAX_DOWNLOAD_SIZE;

    while (total_read < buffer_capacity) {
        int data_read = esp_http_client_read(client, (char *) (buffer + total_read),
                                             buffer_capacity - total_read);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Download error");
            free(buffer);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        } else if (data_read == 0) {
            // End of stream
            break;
        }

        total_read += data_read;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Downloaded %d bytes to RAM", total_read);

    // Validate JPEG header
    if (total_read < 10 || buffer[0] != 0xFF || buffer[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG header: %02X %02X", buffer[0], buffer[1]);
        free(buffer);
        return ESP_FAIL;
    }

    // Check for unsupported JPEG types
    bool is_progressive = false;
    for (int i = 0; i < total_read - 1 && i < 1024; i++) {
        if (buffer[i] == 0xFF && buffer[i + 1] == 0xC2) {
            is_progressive = true;
            ESP_LOGW(TAG, "Progressive JPEG detected - may not be supported");
            break;
        }
    }

    ESP_LOGI(TAG, "JPEG validated (progressive: %d)", is_progressive);

    // ToDo: check timestamp functions
    // Generate filename with timestamp
    time_t now;
    time(&now);

    char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
    album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

    // Ensure directory exists
    struct stat st;
    if (stat(album_path, &st) != 0) {
        ESP_LOGI(TAG, "Creating album directory: %s", album_path);
        mkdir(album_path, 0755);
    }

    char jpg_path[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(jpg_path, sizeof(jpg_path), "%s/telegram_%ld.jpg", album_path, (long) now);

    // Write to SD card
    FILE *fp = fopen(jpg_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to create file: %s", jpg_path);
        free(buffer);
        return ESP_FAIL;
    }

    size_t written = fwrite(buffer, 1, total_read, fp);
    fclose(fp);
    free(buffer);

    if (written != total_read) {
        ESP_LOGE(TAG, "Write error: wrote %zu of %d bytes", written, total_read);
        unlink(jpg_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved to: %s (%d bytes)", jpg_path, total_read);

    // Return path
    strncpy(output_path, jpg_path, output_len - 1);
    output_path[output_len - 1] = '\0';

    return ESP_OK;
}

// Process a single photo message with fallback to smaller sizes
// Helper function to download file to RAM
static esp_err_t download_to_ram(const char *file_path, uint8_t **out_buffer, int *out_size)
{
    char download_url[TELEGRAM_MAX_URL_LENGTH];
    snprintf(download_url, sizeof(download_url), "https://%s/file/bot%s/%s", TELEGRAM_API_BASE,
             bot_token, file_path);
    ESP_LOGI(TAG, "Downloading file from: %s", download_url);

    esp_http_client_config_t dl_config = {
        .url = download_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .buffer_size = TELEGRAM_MAX_DOWN_JPG_SIZE,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t dl_client = esp_http_client_init(&dl_config);
    if (!dl_client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(dl_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(dl_client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(dl_client);
    int status_code = esp_http_client_get_status_code(dl_client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status %d", status_code);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        return ESP_FAIL;
    }

    if (content_length > MAX_DOWNLOAD_SIZE) {
        ESP_LOGE(TAG, "Content too large: %d bytes", content_length);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate buffer in SPIRAM
    int buffer_capacity = content_length > 0 ? content_length : MAX_DOWNLOAD_SIZE;
    uint8_t *ram_buffer = heap_caps_malloc(buffer_capacity, MALLOC_CAP_SPIRAM);
    if (!ram_buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes in SPIRAM", buffer_capacity);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        return ESP_ERR_NO_MEM;
    }

    // Download to RAM
    int total_read = 0;
    while (total_read < buffer_capacity) {
        int data_read = esp_http_client_read(dl_client, (char *) (ram_buffer + total_read),
                                             buffer_capacity - total_read);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Download error");
            free(ram_buffer);
            esp_http_client_close(dl_client);
            esp_http_client_cleanup(dl_client);
            return ESP_FAIL;
        } else if (data_read == 0) {
            break;
        }
        total_read += data_read;
    }

    esp_http_client_close(dl_client);
    esp_http_client_cleanup(dl_client);

    if (total_read < 10) {
        ESP_LOGE(TAG, "Download failed or too small: %d bytes", total_read);
        free(ram_buffer);
        return ESP_FAIL;
    }

    // Validate JPEG header
    if (ram_buffer[0] != 0xFF || ram_buffer[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG header: %02X %02X", ram_buffer[0], ram_buffer[1]);
        free(ram_buffer);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes to RAM", total_read);

    *out_buffer = ram_buffer;
    *out_size = total_read;
    return ESP_OK;
}

// Process a single photo with a specific file_id, trying RAM first (if enabled), then SD fallback
// static esp_err_t process_single_photo(const char *file_id, int photo_index, int array_size) {
static esp_err_t process_single_photo(const char *file_id, int photo_index, int array_size,
                                      const char *base_filename, const char *caption)
{
    ESP_LOGI(TAG, "Processing photo with file_id: %s", file_id);

    // Step 1: Get file path via getFile API
    char getfile_url[TELEGRAM_MAX_URL_LENGTH];
    snprintf(getfile_url, sizeof(getfile_url), "https://%s/bot%s/getFile?file_id=%s",
             TELEGRAM_API_BASE, bot_token, file_id);
    ESP_LOGI(TAG, "Get file path via getFile API from: %s", getfile_url);

    // Reset response buffer
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    http_response_len = 0;
    http_response_capacity = 0;

    esp_http_client_config_t config = {
        .url = getfile_url,
        .event_handler = http_event_handler,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "getFile HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "getFile returned status %d", status_code);
        return ESP_FAIL;
    }

    // Parse response
    http_response_buffer[http_response_len] = '\0';
    cJSON *response = cJSON_Parse(http_response_buffer);
    if (!response) {
        ESP_LOGE(TAG, "Failed to parse getFile response");
        return ESP_FAIL;
    }

    cJSON *ok = cJSON_GetObjectItem(response, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "getFile returned ok=false");
        cJSON_Delete(response);
        return ESP_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(response, "result");
    cJSON *file_path_obj = cJSON_GetObjectItem(result, "file_path");

    if (!file_path_obj || !cJSON_IsString(file_path_obj)) {
        ESP_LOGE(TAG, "No file_path in getFile response");
        cJSON_Delete(response);
        return ESP_FAIL;
    }

    char file_path[TELEGRAM_MAX_PATH_LENGTH];
    // strncpy(file_path, file_path_obj->valuestring, sizeof(file_path) - 1);
    snprintf(file_path, sizeof(file_path) - 1, "%s", file_path_obj->valuestring);
    file_path[sizeof(file_path) - 1] = '\0';

    // Check file size if available
    cJSON *file_size_obj = cJSON_GetObjectItem(result, "file_size");
    if (file_size_obj && cJSON_IsNumber(file_size_obj)) {
        int file_size = file_size_obj->valueint;
        ESP_LOGI(TAG, "Telegram file size: %d bytes", file_size);

        if (file_size > MAX_DOWNLOAD_SIZE) {
            ESP_LOGE(TAG, "File too large: %d bytes (max %d)", file_size, MAX_DOWNLOAD_SIZE);
            cJSON_Delete(response);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    cJSON_Delete(response);
    ESP_LOGI(TAG, "Got file_path: %s", file_path);

    // Step 2: Download to RAM
    ESP_LOGI(TAG, "Downloading to RAM (optimized mode)...");

    uint8_t *ram_buffer = NULL;
    int ram_buffer_size = 0;
    err = download_to_ram(file_path, &ram_buffer, &ram_buffer_size);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RAM download failed: %s, trying SD fallback...", esp_err_to_name(err));

        // ❌ FALLBACK: Download directly to SD
        char jpg_path[TELEGRAM_MAX_PATH_LENGTH];
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
        album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));
        snprintf(jpg_path, sizeof(jpg_path), "%s/%s.jpg", album_path, base_filename);

        err = download_and_save_telegram_file(file_path, jpg_path, sizeof(jpg_path));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD fallback download also failed: %s", esp_err_to_name(err));
            return err;
        }

        // Convert from file (old path)
        char bmp_path[TELEGRAM_MAX_PATH_LENGTH];
        snprintf(bmp_path, sizeof(bmp_path), "%s/%s.bmp", album_path, base_filename);

        ESP_LOGI(TAG, "Converting from SD file (fallback mode): %s", jpg_path);
        err = image_processor_convert_jpg_to_bmp_with_portrait_combine(jpg_path, bmp_path, false,
                                                                       DEFAULT_ALBUM_NAME);

        unlink(jpg_path);  // Delete temp JPG

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD conversion failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Photo processed via SD fallback: %s", bmp_path);

    } else {
        // Step 3: OPTIMIZED PATH: Convert directly from RAM buffer
        ESP_LOGI(TAG, "Downloaded %d bytes to RAM", ram_buffer_size);

        char bmp_filename[TELEGRAM_MAX_FILENAME_LENGTH];
        snprintf(bmp_filename, sizeof(bmp_filename), "%s.bmp", base_filename);

        char bmp_path[TELEGRAM_MAX_PATH_LENGTH];
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
        album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));
        snprintf(bmp_path, sizeof(bmp_path), "%s/%s", album_path, bmp_filename);

        ESP_LOGI(TAG, "Converting buffer to BMP: %s", bmp_path);
        ESP_LOGI(TAG, "Free heap before: %lu bytes, SPIRAM: %lu bytes", esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        // Convert directly from buffer (with portrait combine support)
        // err = image_processor_convert_buffer_to_bmp_with_portrait_combine(
        // ram_buffer, ram_buffer_size, bmp_path, false, DEFAULT_ALBUM_NAME
        //);

        // Save thumbnail BEFORE conversion (so portrait directory check works)
        if (array_size == 1 && ram_buffer != NULL && ram_buffer_size > 0) {
            ESP_LOGI(TAG, "Document mode: saving original JPG as thumbnail...");

            char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
            album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

            // First, save temporarily in main album directory
            char temp_thumb_path[TELEGRAM_MAX_PATH_LENGTH];
            snprintf(temp_thumb_path, sizeof(temp_thumb_path), "%s/%s.jpg", album_path,
                     base_filename);

            FILE *fp = fopen(temp_thumb_path, "wb");
            if (fp) {
                size_t written = fwrite(ram_buffer, 1, ram_buffer_size, fp);
                fclose(fp);

                if (written == (size_t) ram_buffer_size) {
                    ESP_LOGI(TAG, " Thumbnail saved temporarily: %s (%d bytes)", temp_thumb_path,
                             ram_buffer_size);
                } else {
                    ESP_LOGW(TAG, "Thumbnail write incomplete: %zu of %d bytes", written,
                             ram_buffer_size);
                    unlink(temp_thumb_path);
                }
            } else {
                ESP_LOGW(TAG, "Failed to create thumbnail file: %s", temp_thumb_path);
            }
        }

        // Now convert (which might move BMP to portrait directory)
        err = image_processor_convert_buffer_to_bmp_with_portrait_combine(
            ram_buffer, ram_buffer_size, bmp_path, false, DEFAULT_ALBUM_NAME);

        free(ram_buffer);
        ram_buffer = NULL;

        ESP_LOGI(TAG, "Free heap after: %lu bytes, SPIRAM: %lu bytes", esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Buffer conversion failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Photo processed via optimized RAM path: %s", bmp_filename);

        // Move thumbnail to portrait directory if BMP is there
        if (array_size == 1) {
            char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
            album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

            char temp_thumb_path[TELEGRAM_MAX_PATH_LENGTH];
            snprintf(temp_thumb_path, sizeof(temp_thumb_path), "%s/%s.jpg", album_path,
                     base_filename);

            // Check if BMP was saved to portrait directory
            char portrait_check[TELEGRAM_MAX_PATH_LENGTH];
            snprintf(portrait_check, sizeof(portrait_check), "%s/portrait/%s.bmp", album_path,
                     base_filename);

            struct stat st;
            if (stat(portrait_check, &st) == 0) {
                // Portrait BMP exists - move thumbnail there
                ESP_LOGI(TAG, "Portrait detected, moving thumbnail to portrait dir");

                char portrait_thumb_path[TELEGRAM_MAX_PATH_LENGTH];
                snprintf(portrait_thumb_path, sizeof(portrait_thumb_path), "%s/portrait/%s.jpg",
                         album_path, base_filename);

                if (rename(temp_thumb_path, portrait_thumb_path) == 0) {
                    ESP_LOGI(TAG, " Thumbnail moved: %s", portrait_thumb_path);
                } else {
                    ESP_LOGW(TAG, "Failed to move thumbnail to portrait dir");
                }
            } else {
                ESP_LOGI(TAG, "Landscape image, thumbnail stays in main album");
            }
        }

        // Save thumbnail for documents BEFORE freeing buffer
        /*
        if (array_size == 1 && ram_buffer != NULL && ram_buffer_size > 0) {
                ESP_LOGI(TAG, "Document mode: saving original JPG as thumbnail...");

                char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
                album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

                // Check if BMP was saved to portrait directory
                char portrait_check[TELEGRAM_MAX_PATH_LENGTH];
                snprintf(portrait_check, sizeof(portrait_check), "%s/portrait/%s.bmp", album_path,
        base_filename);

                struct stat st;
                bool is_portrait = (stat(portrait_check, &st) == 0);

                // Save thumbnail in same location as BMP
                char thumb_path[TELEGRAM_MAX_PATH_LENGTH];
                if (is_portrait) {
                        snprintf(thumb_path, sizeof(thumb_path), "%s/portrait/%s.jpg", album_path,
        base_filename); ESP_LOGI(TAG, "Portrait detected, saving thumbnail to portrait dir"); } else
        { snprintf(thumb_path, sizeof(thumb_path), "%s/%s.jpg", album_path, base_filename);
                }

                FILE *fp = fopen(thumb_path, "wb");
                if (fp) {
                        size_t written = fwrite(ram_buffer, 1, ram_buffer_size, fp);
                        fclose(fp);

                        if (written == (size_t)ram_buffer_size) {
                                ESP_LOGI(TAG, " Thumbnail saved: %s (%d bytes)", thumb_path,
        ram_buffer_size); } else { ESP_LOGW(TAG, "Thumbnail write incomplete: %zu of %d bytes",
        written, ram_buffer_size); unlink(thumb_path);
                        }
                } else {
                        ESP_LOGW(TAG, "Failed to create thumbnail file: %s", thumb_path);
                }
        }
        */

        // CRITICAL: Free JPG buffer immediately!
        free(ram_buffer);
        ram_buffer = NULL;

        ESP_LOGI(TAG, "Free heap after: %lu bytes, SPIRAM: %lu bytes", esp_get_free_heap_size(),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Buffer conversion failed: %s", esp_err_to_name(err));
            return err;
        }

        ESP_LOGI(TAG, "Photo processed via optimized RAM path: %s", bmp_filename);
        // ESP_LOGI(TAG, "Photo processed via optimized RAM path: %s", base_filename);
        // base_filename = eg "telegram_470"
        // bmp_filename = eg "telegram_470.bmp"

        // Save thumbnail for documents (array_size == 1)
        // For photos with multiple sizes, thumbnail is already saved separately
        /*
        if (array_size == 1) {
                ESP_LOGI(TAG, "Document mode: downloading JPG thumbnail...");

                // Allocate buffer for thumbnail download
                uint8_t *thumb_buffer = heap_caps_malloc(MAX_DOWNLOAD_SIZE, MALLOC_CAP_SPIRAM);
                if (thumb_buffer) {
                        int thumb_size = 0;
                        esp_err_t thumb_err = download_to_ram(file_path, &thumb_buffer,
        &thumb_size);

                        if (thumb_err == ESP_OK && thumb_size > 0) {
                                // Get album path
                                char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
                                album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path,
        sizeof(album_path));

                                // Build thumbnail path: /sdcard/images/Default/telegram_470.jpg
                                char thumb_path[TELEGRAM_MAX_PATH_LENGTH];
                                snprintf(thumb_path, sizeof(thumb_path), "%s/%s.jpg", album_path,
        base_filename);

                                // Write JPG to file
                                FILE *fp = fopen(thumb_path, "wb");
                                if (fp) {
                                        size_t written = fwrite(thumb_buffer, 1, thumb_size, fp);
                                        fclose(fp);

                                        if (written == (size_t)thumb_size) {
                                                ESP_LOGI(TAG, " Thumbnail saved: %s (%d bytes)",
        thumb_path, thumb_size); } else { ESP_LOGW(TAG, "Thumbnail write incomplete: %zu of %d
        bytes", written, thumb_size); unlink(thumb_path);
                                        }
                                } else {
                                        ESP_LOGW(TAG, "Failed to create thumbnail file: %s",
        thumb_path);
                                }
                        } else {
                                ESP_LOGW(TAG, "Failed to download thumbnail: %s",
        esp_err_to_name(thumb_err));
                        }

                        free(thumb_buffer);
                } else {
                        ESP_LOGW(TAG, "Failed to allocate buffer for thumbnail (%d bytes)",
        MAX_DOWNLOAD_SIZE);
                }
        }
        */
        // ---
    }

    // Save caption if provided
    // Save caption as overlay text (filter out display triggers) --> remove_display_triggers
    if (caption && strlen(caption) > 0) {
        ESP_LOGI(TAG, "Saving caption as overlay text...");
        // Note: bmp_path might be in portrait dir, caption handling needs adjustment
        char final_bmp[TELEGRAM_MAX_PATH_LENGTH];
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
        album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));
        snprintf(final_bmp, sizeof(final_bmp), "%s/%s.bmp", album_path, base_filename);
        // display_manager_save_overlay_text(final_bmp, caption);
        char filtered_caption[TELEGRAM_MAX_CAPTION_LENGTH];
        remove_display_triggers(caption, filtered_caption, sizeof(filtered_caption));

        if (strlen(filtered_caption) > 0) {
            ESP_LOGI(TAG, "Saving caption as overlay text: '%s'", filtered_caption);
            display_manager_save_overlay_text(final_bmp, filtered_caption);
        } else {
            ESP_LOGI(TAG, "Caption only contained trigger keywords, skipping overlay");
        }
    }

    // Success!
    ESP_LOGI(TAG, "Telegram photo processed successfully: %s (size: %d/%d)", base_filename,
             photo_index + 1, array_size);

    return ESP_OK;
}

// Download smallest photo as JPEG thumbnail
static esp_err_t download_thumbnail(const char *file_id, const char *output_path)
{
    ESP_LOGI(TAG, "Downloading thumbnail with file_id: %s", file_id);

    // Step 1: Get file path via getFile API
    char getfile_url[TELEGRAM_MAX_URL_LENGTH];
    snprintf(getfile_url, sizeof(getfile_url), "https://%s/bot%s/getFile?file_id=%s",
             TELEGRAM_API_BASE, bot_token, file_id);

    // Reset response buffer
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    http_response_len = 0;
    http_response_capacity = 0;

    esp_http_client_config_t config = {
        .url = getfile_url,
        .event_handler = http_event_handler,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TELEGRAM_SHORT_TIMEOUT_MS,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    // esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "getFile HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "getFile returned status %d", status_code);
        return ESP_FAIL;
    }

    // Parse response
    http_response_buffer[http_response_len] = '\0';
    cJSON *response = cJSON_Parse(http_response_buffer);
    if (!response) {
        ESP_LOGE(TAG, "Failed to parse getFile response");
        return ESP_FAIL;
    }

    cJSON *ok = cJSON_GetObjectItem(response, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "getFile returned ok=false");
        cJSON_Delete(response);
        return ESP_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(response, "result");
    cJSON *file_path_obj = cJSON_GetObjectItem(result, "file_path");
    if (!file_path_obj || !cJSON_IsString(file_path_obj)) {
        ESP_LOGE(TAG, "No file_path in getFile response");
        cJSON_Delete(response);
        return ESP_FAIL;
    }

    char file_path[TELEGRAM_MAX_PATH_LENGTH];
    strncpy(file_path, file_path_obj->valuestring, sizeof(file_path) - 1);
    file_path[sizeof(file_path) - 1] = '\0';
    cJSON_Delete(response);

    // Step 2: Download thumbnail file
    char download_url[TELEGRAM_MAX_URL_LENGTH];
    snprintf(download_url, sizeof(download_url), "https://%s/file/bot%s/%s", TELEGRAM_API_BASE,
             bot_token, file_path);

    ESP_LOGI(TAG, "Downloading thumbnail from: %s", download_url);

    esp_http_client_config_t dl_config = {
        .url = download_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = TELEGRAM_HTTP_TIMEOUT_MS,
        .buffer_size = TELEGRAM_MAX_DOWN_JPG_SIZE,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t dl_client = esp_http_client_init(&dl_config);
    if (!dl_client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP dl_client");
        return ESP_FAIL;
    }
    err = esp_http_client_open(dl_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(dl_client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(dl_client);
    status_code = esp_http_client_get_status_code(dl_client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET failed with status %d", status_code);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        return ESP_FAIL;
    }

    if (content_length > MAX_DOWNLOAD_SIZE) {
        ESP_LOGE(TAG, "Thumbnail too large: %d bytes", content_length);
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        return ESP_ERR_INVALID_SIZE;
    }

    // Allocate buffer
    int buffer_capacity =
        content_length > 0 ? content_length : (512 * 1024);  // Max 512KB for thumbnail
    uint8_t *buffer = heap_caps_malloc(buffer_capacity, MALLOC_CAP_SPIRAM);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate buffer for thumbnail");
        esp_http_client_close(dl_client);
        esp_http_client_cleanup(dl_client);
        return ESP_ERR_NO_MEM;
    }

    // Download to RAM
    int total_read = 0;
    while (total_read < buffer_capacity) {
        int data_read = esp_http_client_read(dl_client, (char *) (buffer + total_read),
                                             buffer_capacity - total_read);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Download error");
            free(buffer);
            esp_http_client_close(dl_client);
            esp_http_client_cleanup(dl_client);
            return ESP_FAIL;
        } else if (data_read == 0) {
            break;
        }
        total_read += data_read;
    }

    esp_http_client_close(dl_client);
    esp_http_client_cleanup(dl_client);

    if (total_read < 10) {
        ESP_LOGE(TAG, "Download failed or too small: %d bytes", total_read);
        free(buffer);
        return ESP_FAIL;
    }

    // Validate JPEG header
    if (buffer[0] != 0xFF || buffer[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG header for thumbnail: %02X %02X", buffer[0], buffer[1]);
        free(buffer);
        return ESP_FAIL;
    }

    // Write to file
    FILE *fp = fopen(output_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to create thumbnail file: %s", output_path);
        free(buffer);
        return ESP_FAIL;
    }

    size_t written = fwrite(buffer, 1, total_read, fp);
    fclose(fp);
    free(buffer);

    if (written != total_read) {
        ESP_LOGE(TAG, "Write error: wrote %zu of %d bytes", written, total_read);
        unlink(output_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Thumbnail saved: %s (%d bytes)", output_path, total_read);
    return ESP_OK;
}

// Process a single photo message with fallback to smaller sizes
static esp_err_t process_photo_message(cJSON *message)
{
    cJSON *photo_array = cJSON_GetObjectItem(message, "photo");
    if (!photo_array || !cJSON_IsArray(photo_array)) {
        ESP_LOGW(TAG, "Message has no photo array");
        return ESP_FAIL;
    }

    int array_size = cJSON_GetArraySize(photo_array);
    if (array_size == 0) {
        ESP_LOGW(TAG, "Photo array is empty");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Photo array contains %d sizes", array_size);

    // Generate filename with timestamp (will be used for both BMP and thumbnail)
    // time_t now;
    // time(&now);
    char base_filename[TELEGRAM_MAX_FILENAME_LENGTH];
    // snprintf(base_filename, sizeof(base_filename), "telegram_%ld", (long)now);

    // Get file_unique_id from photo object (use largest photo)
    cJSON *largest_photo = cJSON_GetArrayItem(photo_array, array_size - 1);
    cJSON *file_unique_id_obj = cJSON_GetObjectItem(largest_photo, "file_unique_id");

    if (file_unique_id_obj && cJSON_IsString(file_unique_id_obj)) {
        const char *file_unique_id = file_unique_id_obj->valuestring;

        // Sanitize file_unique_id (remove special chars, keep only alphanumeric)
        char sanitized_id[64] = {0};
        int j = 0;
        for (int i = 0; file_unique_id[i] && j < 63; i++) {
            char c = file_unique_id[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-') {
                sanitized_id[j++] = c;
            }
        }
        sanitized_id[j] = '\0';

        snprintf(base_filename, sizeof(base_filename), "tg_%s", sanitized_id);
        ESP_LOGI(TAG, "Using file_unique_id as filename: %s", base_filename);
    } else {
        // Fallback to timestamp if file_unique_id not available
        time_t now = time(NULL);
        snprintf(base_filename, sizeof(base_filename), "telegram_%ld", (long) now);
        ESP_LOGW(TAG, "file_unique_id not available, using timestamp: %s", base_filename);
    }

    // *** HIER JETZT DEN DUPLIKAT-CHECK EINFÜGEN ***

    // Check if file already exists (automatic duplicate detection)
    // char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
    // album_manager_get_album_path(album_name, album_path, sizeof(album_path));

    // Extract chat_id for notifications
    int64_t chat_id = 0;
    cJSON *chat_obj = cJSON_GetObjectItem(message, "chat");
    if (chat_obj) {
        cJSON *chat_id_obj = cJSON_GetObjectItem(chat_obj, "id");
        if (chat_id_obj && cJSON_IsNumber(chat_id_obj)) {
            chat_id = (int64_t) chat_id_obj->valuedouble;
        }
    }

    // Check if file already exists (automatic duplicate detection)
    char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
    album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

    char existing_bmp[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(existing_bmp, sizeof(existing_bmp), "%s/%s.bmp", album_path, base_filename);

    struct stat st_dup;
    if (stat(existing_bmp, &st_dup) == 0) {
        ESP_LOGW(TAG, "Image already exists: %s (duplicate detected)", existing_bmp);

        // Send notification to user
        if (chat_id != 0) {
            telegram_bot_send_message(chat_id,
                                      "ℹ️ <b>Duplicate Detected</b>\nThis image was already "
                                      "uploaded previously. Skipping.");
        }

        // Return ESP_ERR_INVALID_STATE instead of ESP_OK
        return ESP_ERR_INVALID_STATE;  // Skip processing
    }

    // Also check portrait directory
    char portrait_bmp[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(portrait_bmp, sizeof(portrait_bmp), "%s/portrait/%s.bmp", album_path, base_filename);

    if (stat(portrait_bmp, &st_dup) == 0) {
        ESP_LOGW(TAG, "Portrait image already exists: %s (duplicate detected)", portrait_bmp);

        if (chat_id != 0) {
            telegram_bot_send_message(chat_id,
                                      "ℹ️ <b>Duplicate Detected</b>\nThis image was already "
                                      "uploaded previously. Skipping.");
        }

        return ESP_ERR_INVALID_STATE;  // ESP_OK
    }

    ESP_LOGI(TAG, " New image detected (not a duplicate)");

    // char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
    // album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

    // Ensure directory exists
    struct stat st;
    if (stat(album_path, &st) != 0) {
        mkdir(album_path, 0755);
    }

    // Step 1: Download smallest photo as thumbnail (first element in array)
    cJSON *thumbnail_photo = cJSON_GetArrayItem(photo_array, 0);
    cJSON *thumb_file_id_obj = cJSON_GetObjectItem(thumbnail_photo, "file_id");

    char thumbnail_path[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(thumbnail_path, sizeof(thumbnail_path), "%s/%s.jpg", album_path, base_filename);

    esp_err_t thumb_err = ESP_FAIL;
    if (thumb_file_id_obj && cJSON_IsString(thumb_file_id_obj)) {
        ESP_LOGI(TAG, "Downloading thumbnail (size 1/%d)", array_size);
        thumb_err = download_thumbnail(thumb_file_id_obj->valuestring, thumbnail_path);
        if (thumb_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to download thumbnail, will continue with main image");
        } else {
            ESP_LOGI(TAG, "Thumbnail downloaded successfully: %s", thumbnail_path);
        }
    }

    // Step 2: Process main image (try from largest to smallest until one works)
    esp_err_t main_err = ESP_FAIL;
    for (int attempt = 0; attempt < array_size; attempt++) {
        // Start with second-largest (index = array_size - 2), then go smaller
        // Skip the very largest on first try to avoid Progressive JPEG
        int photo_index;
        if (attempt == 0 && array_size > 1) {
            photo_index = array_size - 2;  // Second largest first
            ESP_LOGI(TAG, "Trying second-largest photo (index %d/%d)", photo_index + 1, array_size);
        } else if (attempt == 1 && array_size > 1) {
            photo_index = array_size - 1;  // Then try largest
            ESP_LOGI(TAG, "Retrying with largest photo (index %d/%d)", photo_index + 1, array_size);
        } else {
            photo_index = array_size - 1 - attempt;  // Then go smaller
            if (photo_index < 0)
                break;
            ESP_LOGI(TAG, "Retrying with smaller photo (index %d/%d)", photo_index + 1, array_size);
        }

        cJSON *photo = cJSON_GetArrayItem(photo_array, photo_index);
        cJSON *file_id_obj = cJSON_GetObjectItem(photo, "file_id");
        if (!file_id_obj || !cJSON_IsString(file_id_obj)) {
            ESP_LOGE(TAG, "No file_id in photo at index %d", photo_index);
            continue;
        }

        const char *file_id = file_id_obj->valuestring;

        const char *caption = NULL;
        cJSON *caption_obj = cJSON_GetObjectItem(message, "caption");
        if (caption_obj && cJSON_IsString(caption_obj)) {
            caption = caption_obj->valuestring;
            ESP_LOGI(TAG, "Photo caption: %s", caption);
        }

        main_err = process_single_photo(file_id, photo_index, array_size, base_filename, caption);

        if (main_err == ESP_OK) {
            // Check if BMP was saved as portrait and move thumbnail accordingly
            char portrait_bmp_check[TELEGRAM_MAX_PATH_LENGTH];
            snprintf(portrait_bmp_check, sizeof(portrait_bmp_check), "%s/portrait/%s.bmp",
                     album_path, base_filename);

            struct stat st_portrait;
            if (stat(portrait_bmp_check, &st_portrait) == 0) {
                // Portrait BMP exists - move thumbnail to portrait directory
                ESP_LOGI(TAG, "Portrait BMP detected, moving thumbnail to portrait dir");

                char new_thumbnail_path[TELEGRAM_MAX_PATH_LENGTH];
                snprintf(new_thumbnail_path, sizeof(new_thumbnail_path), "%s/portrait/%s.jpg",
                         album_path, base_filename);

                // Ensure portrait directory exists
                char portrait_dir[TELEGRAM_MAX_PATH_LENGTH];
                snprintf(portrait_dir, sizeof(portrait_dir), "%s/portrait", album_path);
                if (stat(portrait_dir, &st) != 0) {
                    mkdir(portrait_dir, 0755);
                }

                // Move thumbnail
                if (rename(thumbnail_path, new_thumbnail_path) == 0) {
                    ESP_LOGI(TAG, " Thumbnail moved to: %s", new_thumbnail_path);
                } else {
                    ESP_LOGW(TAG, "Failed to move thumbnail to portrait dir");
                }
            }

            ESP_LOGI(TAG, " Photo processed successfully: %s.bmp (with thumbnail: %s.jpg)",
                     base_filename, base_filename);

            // Check if should display immediately
            if (caption && should_display_immediately(caption)) {
                ESP_LOGI(TAG, "Caption contains display trigger - showing image now");

                // Get album path
                char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
                album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

                char display_bmp[TELEGRAM_MAX_PATH_LENGTH];
                bool found_image = false;

                // Priority 1: Main album (newly uploaded)
                snprintf(display_bmp, sizeof(display_bmp), "%s/%s.bmp", album_path, base_filename);
                struct stat st_check;
                if (stat(display_bmp, &st_check) == 0) {
                    ESP_LOGI(TAG, "Found image in main album: %s", display_bmp);
                    found_image = true;
                }

                // Priority 2: Portrait directory
                if (!found_image) {
                    snprintf(display_bmp, sizeof(display_bmp), "%s/portrait/%s.bmp", album_path,
                             base_filename);
                    if (stat(display_bmp, &st_check) == 0) {
                        ESP_LOGI(TAG, "Found image in portrait dir: %s", display_bmp);
                        found_image = true;
                    }
                }

                // Priority 3: Recently combined image (fallback)
                if (!found_image) {
                    DIR *dir = opendir(album_path);
                    if (dir) {
                        struct dirent *entry;
                        time_t newest_time = 0;

                        while ((entry = readdir(dir)) != NULL) {
                            if (strncmp(entry->d_name, "combined_", 9) == 0 &&
                                strstr(entry->d_name, ".bmp") != NULL) {
                                char full_path[TELEGRAM_MAX_PATH_LENGTH];
                                snprintf(full_path, sizeof(full_path), "%s/%s", album_path,
                                         entry->d_name);

                                struct stat file_stat;
                                if (stat(full_path, &file_stat) == 0) {
                                    if ((time(NULL) - file_stat.st_mtime) < 30) {
                                        if (file_stat.st_mtime > newest_time) {
                                            newest_time = file_stat.st_mtime;
                                            strncpy(display_bmp, full_path,
                                                    sizeof(display_bmp) - 1);
                                            found_image = true;
                                        }
                                    }
                                }
                            }
                        }
                        closedir(dir);

                        if (found_image) {
                            ESP_LOGI(TAG, "Found recently combined image: %s", display_bmp);
                        }
                    }
                }

                // Display the image
                if (found_image) {
                    ESP_LOGI(TAG, "Displaying image: %s", display_bmp);
                    display_manager_show_image(display_bmp);
                } else {
                    ESP_LOGW(TAG, "No BMP file found for display");
                }
            }

            return ESP_OK;  // Success!

            /*
            ESP_LOGI(TAG, " Photo processed successfully: %s.bmp (with thumbnail: %s.jpg)",
                     base_filename, base_filename);

            // Check if should display immediately
            if (caption && should_display_immediately(caption)) {
                ESP_LOGI(TAG, "Caption contains display trigger - showing image now");

                // Build full path to BMP
                char display_bmp[TELEGRAM_MAX_PATH_LENGTH];

                // Check if in portrait directory
                struct stat st_check;
                snprintf(display_bmp, sizeof(display_bmp), "%s/portrait/%s.bmp", album_path,
            base_filename);

                if (stat(display_bmp, &st_check) != 0) {
                    // Not in portrait dir, try main album
                    snprintf(display_bmp, sizeof(display_bmp), "%s/%s.bmp", album_path,
            base_filename);
                }

                // Check if combined image exists (for portrait pairs)
                char combined_pattern[TELEGRAM_MAX_PATH_LENGTH];
                snprintf(combined_pattern, sizeof(combined_pattern), "%s/combined_", album_path);

                // Try to find most recent combined image
                DIR *dir = opendir(album_path);
                if (dir) {
                    struct dirent *entry;
                    time_t newest_time = 0;
                    char newest_combined[TELEGRAM_MAX_PATH_LENGTH] = {0};

                    while ((entry = readdir(dir)) != NULL) {
                        if (strncmp(entry->d_name, "combined_", 9) == 0) {
                            char full_path[TELEGRAM_MAX_PATH_LENGTH];
                            snprintf(full_path, sizeof(full_path), "%s/%s", album_path,
            entry->d_name);

                            struct stat file_stat;
                            if (stat(full_path, &file_stat) == 0) {
                                if (file_stat.st_mtime > newest_time) {
                                    newest_time = file_stat.st_mtime;
                                    strncpy(newest_combined, full_path, sizeof(newest_combined) -
            1);
                                }
                            }
                        }
                    }
                    closedir(dir);

                    // If we found a recent combined image (within last 5 seconds), use it
                    if (newest_time > 0 && (time(NULL) - newest_time) < 5) {
                        strncpy(display_bmp, newest_combined, sizeof(display_bmp) - 1);
                        ESP_LOGI(TAG, "Using recently combined image: %s", display_bmp);
                    }
                }

                // Check if file exists
                if (stat(display_bmp, &st_check) == 0) {
                    ESP_LOGI(TAG, "Displaying image: %s", display_bmp);
                    display_manager_show_image(display_bmp);
                } else {
                    ESP_LOGW(TAG, "BMP file not found for display: %s", display_bmp);
                }
            }

            return ESP_OK; // Success!
                        */
        }

        // If conversion failed, try smaller size
        if (main_err == ESP_FAIL && attempt < array_size - 1) {
            ESP_LOGI(TAG, "Likely Progressive JPEG or conversion error, trying smaller size...");
            continue;
        }

        // Other errors might be fatal
        if (main_err != ESP_FAIL && main_err != ESP_ERR_INVALID_SIZE) {
            break;
        }
    }

    // If main image failed but thumbnail succeeded, keep the thumbnail
    if (main_err != ESP_OK && thumb_err == ESP_OK) {
        ESP_LOGW(TAG, "Main image processing failed, but thumbnail was saved");
        return ESP_FAIL;  // Still report failure for main image
    }

    // If main image failed and thumbnail failed, clean up thumbnail
    if (main_err != ESP_OK && thumb_err != ESP_OK) {
        unlink(thumbnail_path);  // Clean up failed thumbnail
    }

    // All sizes failed
    ESP_LOGE(TAG, "Failed to process photo: all %d sizes failed", array_size);
    return ESP_FAIL;
}

/**
 * @brief Process a document message (image sent as file)
 * Documents use same getFile API as photos
 */
static esp_err_t process_document_as_photo(cJSON *message)
{
    ESP_LOGI(TAG, "=== PROCESS DOCUMENT AS PHOTO ===");

    cJSON *document = cJSON_GetObjectItem(message, "document");
    if (!document || !cJSON_IsObject(document)) {
        ESP_LOGW(TAG, "Message has no document");
        return ESP_FAIL;
    }

    // Get file_id from document
    cJSON *file_id_obj = cJSON_GetObjectItem(document, "file_id");
    if (!file_id_obj || !cJSON_IsString(file_id_obj)) {
        ESP_LOGE(TAG, "No file_id in document");
        return ESP_FAIL;
    }

    const char *file_id = file_id_obj->valuestring;
    ESP_LOGI(TAG, "Document file_id: %s", file_id);

    // Check file size
    cJSON *file_size_obj = cJSON_GetObjectItem(document, "file_size");
    if (file_size_obj && cJSON_IsNumber(file_size_obj)) {
        int file_size = file_size_obj->valueint;
        ESP_LOGI(TAG, "Document file size: %d bytes", file_size);

        if (file_size > MAX_DOWNLOAD_SIZE) {
            ESP_LOGE(TAG, "Document too large: %d bytes (max %d)", file_size, MAX_DOWNLOAD_SIZE);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    // Extract chat_id for notifications
    int64_t chat_id = 0;
    cJSON *chat_obj = cJSON_GetObjectItem(message, "chat");
    if (chat_obj) {
        cJSON *chat_id_obj = cJSON_GetObjectItem(chat_obj, "id");
        if (chat_id_obj && cJSON_IsNumber(chat_id_obj)) {
            chat_id = (int64_t) chat_id_obj->valuedouble;
        }
    }

    // Generate filename using file_unique_id for automatic duplicate detection
    char base_filename[TELEGRAM_MAX_FILENAME_LENGTH];

    // Get file_unique_id from document
    cJSON *file_unique_id_obj = cJSON_GetObjectItem(document, "file_unique_id");

    if (file_unique_id_obj && cJSON_IsString(file_unique_id_obj)) {
        const char *file_unique_id = file_unique_id_obj->valuestring;

        // Sanitize file_unique_id (remove special chars)
        char sanitized_id[64] = {0};
        int j = 0;
        for (int i = 0; file_unique_id[i] && j < 63; i++) {
            char c = file_unique_id[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-') {
                sanitized_id[j++] = c;
            }
        }
        sanitized_id[j] = '\0';

        snprintf(base_filename, sizeof(base_filename), "tg_%s", sanitized_id);
        ESP_LOGI(TAG, "Using file_unique_id as filename: %s", base_filename);
    } else {
        // Fallback to timestamp
        time_t now = time(NULL);
        snprintf(base_filename, sizeof(base_filename), "telegram_%ld", (long) now);
        ESP_LOGW(TAG, "file_unique_id not available, using timestamp: %s", base_filename);
    }

    ESP_LOGI(TAG, "Generated filename: %s", base_filename);

    // Check if file already exists (automatic duplicate detection)
    char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
    album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

    char existing_bmp[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(existing_bmp, sizeof(existing_bmp), "%s/%s.bmp", album_path, base_filename);

    struct stat st_dup;
    if (stat(existing_bmp, &st_dup) == 0) {
        ESP_LOGW(TAG, "Document already exists: %s (duplicate detected)", existing_bmp);

        if (chat_id != 0) {
            telegram_bot_send_message(chat_id,
                                      "ℹ️ <b>Duplicate Detected</b>\nThis document was already "
                                      "uploaded previously. Skipping.");
        }

        return ESP_ERR_INVALID_STATE;  // ← ESP_OK
    }

    // Also check portrait directory
    char portrait_bmp[TELEGRAM_MAX_PATH_LENGTH];
    snprintf(portrait_bmp, sizeof(portrait_bmp), "%s/portrait/%s.bmp", album_path, base_filename);

    if (stat(portrait_bmp, &st_dup) == 0) {
        ESP_LOGW(TAG, "Portrait document already exists: %s", portrait_bmp);

        if (chat_id != 0) {
            telegram_bot_send_message(chat_id,
                                      "ℹ️ <b>Duplicate Detected</b>\nThis document was already "
                                      "uploaded previously. Skipping.");
        }

        return ESP_ERR_INVALID_STATE;  // ← ESP_OK
    }

    ESP_LOGI(TAG, " New document detected (not a duplicate)");

    // Get caption if provided
    const char *caption = NULL;
    cJSON *caption_obj = cJSON_GetObjectItem(message, "caption");
    if (caption_obj && cJSON_IsString(caption_obj)) {
        caption = caption_obj->valuestring;
        ESP_LOGI(TAG, "Document caption: %s", caption);
    }

    // Process the document using process_single_photo
    // Use index 0 and arraysize 1 since documents have only one file
    esp_err_t err = process_single_photo(file_id, 0, 1, base_filename, caption);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, " Document processed successfully: %s.bmp", base_filename);

        // Check if should display immediately
        if (caption && should_display_immediately(caption)) {
            ESP_LOGI(TAG, "Caption contains display trigger - showing image now");

            // Get album path
            char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
            album_manager_get_album_path(DEFAULT_ALBUM_NAME, album_path, sizeof(album_path));

            char display_bmp[TELEGRAM_MAX_PATH_LENGTH];
            bool found_image = false;

            // Priority 1: Main album (newly uploaded)
            snprintf(display_bmp, sizeof(display_bmp), "%s/%s.bmp", album_path, base_filename);
            struct stat st_check;
            if (stat(display_bmp, &st_check) == 0) {
                ESP_LOGI(TAG, "Found image in main album: %s", display_bmp);
                found_image = true;
            }

            // Priority 2: Portrait directory
            if (!found_image) {
                snprintf(display_bmp, sizeof(display_bmp), "%s/portrait/%s.bmp", album_path,
                         base_filename);
                if (stat(display_bmp, &st_check) == 0) {
                    ESP_LOGI(TAG, "Found image in portrait dir: %s", display_bmp);
                    found_image = true;
                }
            }

            // Priority 3: Recently combined image (fallback)
            if (!found_image) {
                DIR *dir = opendir(album_path);
                if (dir) {
                    struct dirent *entry;
                    time_t newest_time = 0;

                    while ((entry = readdir(dir)) != NULL) {
                        if (strncmp(entry->d_name, "combined_", 9) == 0 &&
                            strstr(entry->d_name, ".bmp") != NULL) {
                            char full_path[TELEGRAM_MAX_PATH_LENGTH];
                            snprintf(full_path, sizeof(full_path), "%s/%s", album_path,
                                     entry->d_name);

                            struct stat file_stat;
                            if (stat(full_path, &file_stat) == 0) {
                                if ((time(NULL) - file_stat.st_mtime) < 30) {
                                    if (file_stat.st_mtime > newest_time) {
                                        newest_time = file_stat.st_mtime;
                                        strncpy(display_bmp, full_path, sizeof(display_bmp) - 1);
                                        found_image = true;
                                    }
                                }
                            }
                        }
                    }
                    closedir(dir);

                    if (found_image) {
                        ESP_LOGI(TAG, "Found recently combined image: %s", display_bmp);
                    }
                }
            }

            // Display the image
            if (found_image) {
                ESP_LOGI(TAG, "Displaying image: %s", display_bmp);
                display_manager_show_image(display_bmp);
            } else {
                ESP_LOGW(TAG, "No BMP file found for display");
            }
        }
    } else {
        ESP_LOGE(TAG, "Document processing failed: %s", esp_err_to_name(err));
    }

    return err;
}

// CMD handler
// Helper: Remove bot name from command (e.g., "/sleep@BotName" -> "/sleep")
static void strip_bot_name(char *command)
{
    char *at_sign = strchr(command, '@');
    if (at_sign) {
        *at_sign = '\0';  // Terminate string at @
    }
}
/*
int64_t telegram_bot_get_last_chat_id(void) {
    return last_chat_id;
}
*/

/**
 * @brief Handle /combine command for portrait combine feature
 *
 * Commands:
 *   /combine         - Show current status
 *   /combine on      - Enable portrait combine
 *   /combine off     - Disable portrait combine
 *   /combine enable  - Enable portrait combine (alias)
 *   /combine disable - Disable portrait combine (alias)
 */
// Portrait Combine Command Handler
static void handle_portrait_combine_command(const char *text, int64_t chat_id)
{
    // remove Bot name ("/combine@BotName" -> "/combine")
    char text_buffer[TELEGRAM_MAX_MESSAGE_LENGTH_XS];
    strncpy(text_buffer, text, sizeof(text_buffer) - 1);
    text_buffer[sizeof(text_buffer) - 1] = '\0';
    strip_bot_name(text_buffer);

    // Nutze gereinigten Text
    const char *cleaned_text = text_buffer;

    // Skip "/combine" command
    const char *param = cleaned_text;
    if (strncmp(param, "/combine", 8) == 0) {
        param += 8;  // Skip "/combine"
    }

    // Skip whitespace
    while (*param == ' ')
        param++;

    char response[256];
    bool current_state = image_processor_get_portrait_combine_enabled();

    if (*param == '\0') {
        // No parameter - show current status
        snprintf(response, sizeof(response),
                 "🖼️ <b>Portrait Combine: %s</b>\n\n"
                 "Use:\n"
                 "• <code>/combine on</code> - Enable\n"
                 "• <code>/combine off</code> - Disable",
                 current_state ? "ENABLED" : "DISABLED");
    } else if (strcmp(param, "on") == 0 || strcmp(param, "enable") == 0) {
        // Enable portrait combine
        image_processor_set_portrait_combine_enabled(true);
        snprintf(response, sizeof(response),
                 "✅ <b>Portrait Combine ENABLED</b>\n\n"
                 "Portrait photos will be:\n"
                 "• Saved unrotated to <code>/portrait</code>\n"
                 "• Combined when 2nd portrait arrives");
    } else if (strcmp(param, "off") == 0 || strcmp(param, "disable") == 0) {
        // Disable portrait combine
        image_processor_set_portrait_combine_enabled(false);
        snprintf(response, sizeof(response),
                 "❌ <b>Portrait Combine DISABLED</b>\n\n"
                 "Portrait photos will be:\n"
                 "• Rotated to landscape\n"
                 "• Saved to main album");
    } else {
        // Invalid parameter
        snprintf(response, sizeof(response),
                 "⚠️ <b>Invalid command</b>\n\n"
                 "Use:\n"
                 "• <code>/combine</code> - Show status\n"
                 "• <code>/combine on</code> - Enable\n"
                 "• <code>/combine off</code> - Disable");
    }

    telegram_bot_send_message(chat_id, response);
}

esp_err_t get_public_ip_https(char *ip_buffer, size_t buffer_size)
{
    char local_buffer[64] = {0};

    esp_http_client_config_t config = {
        .url = "https://api.ipify.org",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    // 1. Verbindung öffnen
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE("PUBLIC_IP", "Failed to open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        snprintf(ip_buffer, buffer_size, "Error");
        return err;
    }

    // 2. Headers holen
    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    ESP_LOGI("PUBLIC_IP", "Status: %d, Length: %d", status_code, content_length);

    // 3. Body lesen
    if (status_code == 200 && content_length > 0 && content_length < sizeof(local_buffer)) {
        int read_len = esp_http_client_read(client, local_buffer, content_length);

        ESP_LOGI("PUBLIC_IP", "Read %d bytes", read_len);

        if (read_len > 0) {
            local_buffer[read_len] = '\0';
            snprintf(ip_buffer, buffer_size, "%s", local_buffer);
            ESP_LOGI("PUBLIC_IP", "Public IP: %s", ip_buffer);
        } else {
            ESP_LOGE("PUBLIC_IP", "Read returned: %d", read_len);
            snprintf(ip_buffer, buffer_size, "Error");
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE("PUBLIC_IP", "Bad response");
        snprintf(ip_buffer, buffer_size, "Error");
        err = ESP_FAIL;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// Status Information
static void handle_status_command(int64_t chat_id)
{
    char response[1536];

    bool portrait_combine = image_processor_get_portrait_combine_enabled();

    // Memory
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint32_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t total_spiram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t total_dma = heap_caps_get_total_size(MALLOC_CAP_DMA);

    // Largest free blocks
    uint32_t largest_free = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t largest_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // Peak usage
    uint32_t peak_usage = total_heap - min_free_heap;

    // Percentages
    uint32_t heap_used_pct = ((total_heap - free_heap) * 100) / total_heap;
    uint32_t spiram_used_pct =
        total_spiram > 0 ? ((total_spiram - free_spiram) * 100) / total_spiram : 0;
    uint32_t internal_used_pct = ((total_internal - free_internal) * 100) / total_internal;

    // Öffentliche IP abrufen
    // IP abrufen (non-blocking durch timeout)
    char public_ip[64] = "N/A";
    esp_err_t err = get_public_ip_https(public_ip, sizeof(public_ip));
    if (err != ESP_OK) {
        snprintf(public_ip, sizeof(public_ip), "Error");
    }

    // Lokale IP-Adresse abrufen
    char local_ip[16] = "N/A";
    esp_netif_ip_info_t ip_info;
    // char local_ip[16] = "N/A";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        // sprintf(local_ip, IPSTR, IP2STR(&ip_info.ip));
        snprintf(local_ip, sizeof(local_ip), IPSTR, IP2STR(&ip_info.ip));
    }

    snprintf(response, sizeof(response),
             "📊 <b>System Status</b>\n\n"

             "<b>🌐 Network</b>\n"
             "Local IP: <code>%s</code>\n"
             "Public IP: <code>%s</code>\n\n"

             "<b>🔄 Processing Mode</b>\n"
             "Portrait Combine: %s\n\n"

             "<b>💾 Total Heap</b>\n"
             "Free: <code>%lu / %lu KB</code> (%lu%%)\n"
             "Peak Usage: <code>%lu KB</code>\n"
             "Largest Block: <code>%lu KB</code>\n\n"

             "<b>🚀 SPIRAM</b>\n"
             "Free: <code>%lu / %lu KB</code> (%lu%%)\n"
             "Largest Block: <code>%lu KB</code>\n\n"

             "<b>🧠 Internal RAM</b>\n"
             "Free: <code>%lu / %lu KB</code> (%lu%%)\n\n"

             "<b>⚡ DMA Memory</b>\n"
             "Free: <code>%lu / %lu KB</code>",
             local_ip, public_ip, portrait_combine ? "✅ Enabled" : "❌ Disabled",

             (unsigned long) (free_heap / 1024), (unsigned long) (total_heap / 1024),
             (unsigned long) heap_used_pct, (unsigned long) (peak_usage / 1024),
             (unsigned long) (largest_free / 1024),

             (unsigned long) (free_spiram / 1024), (unsigned long) (total_spiram / 1024),
             (unsigned long) spiram_used_pct, (unsigned long) (largest_spiram / 1024),

             (unsigned long) (free_internal / 1024), (unsigned long) (total_internal / 1024),
             (unsigned long) internal_used_pct,

             (unsigned long) (free_dma / 1024), (unsigned long) (total_dma / 1024));

    telegram_bot_send_message(chat_id, response);
}

/*
static void handle_status_command(int64_t chat_id) {
    char response[1024];

    bool portrait_combine = image_processor_get_portrait_combine_enabled();

    // Memory info
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_free_heap = esp_get_minimum_free_heap_size();
    uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);

    uint32_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t total_spiram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    uint32_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint32_t total_dma = heap_caps_get_total_size(MALLOC_CAP_DMA);

    // Calculate usage percentages
    uint32_t heap_used_percent = 0;
    if (total_heap > 0) {
        heap_used_percent = ((total_heap - free_heap) * 100) / total_heap;
    }

    uint32_t spiram_used_percent = 0;
    if (total_spiram > 0) {
        spiram_used_percent = ((total_spiram - free_spiram) * 100) / total_spiram;
    }

    uint32_t internal_used_percent = 0;
    if (total_internal > 0) {
        internal_used_percent = ((total_internal - free_internal) * 100) / total_internal;
    }

    // Build response
    snprintf(response, sizeof(response),
             "📊 *System Status*\n\n"

             "*Processing:*\n"
             "Portrait Combine: %s\n\n"

             "*Total Heap:*\n"
             "Free: %lu KB / %lu KB (%lu%%)\n"
             "Min Free: %lu KB\n\n"

             "*SPIRAM (PSRAM):*\n"
             "Free: %lu KB / %lu KB (%lu%%)\n"
             "Used: %lu KB\n\n"

             "*Internal RAM:*\n"
             "Free: %lu KB / %lu KB (%lu%%)\n"
             "Used: %lu KB\n\n"

             "*DMA Capable:*\n"
             "Free: %lu KB / %lu KB",

             // Portrait combine
             portrait_combine ? "Enabled" : "Disabled",

             // Total heap
             (unsigned long)(free_heap / 1024),
             (unsigned long)(total_heap / 1024),
             (unsigned long)heap_used_percent,
             (unsigned long)(min_free_heap / 1024),

             // SPIRAM
             (unsigned long)(free_spiram / 1024),
             (unsigned long)(total_spiram / 1024),
             (unsigned long)spiram_used_percent,
             (unsigned long)((total_spiram - free_spiram) / 1024),

             // Internal RAM
             (unsigned long)(free_internal / 1024),
             (unsigned long)(total_internal / 1024),
             (unsigned long)internal_used_percent,
             (unsigned long)((total_internal - free_internal) / 1024),

             // DMA
             (unsigned long)(free_dma / 1024),
             (unsigned long)(total_dma / 1024));

    telegram_bot_send_message(chat_id, response);
}
*/

static void handle_telegram_command(cJSON *message)
{
    cJSON *text_obj = cJSON_GetObjectItem(message, "text");
    cJSON *chat_obj = cJSON_GetObjectItem(message, "chat");

    if (!text_obj || !cJSON_IsString(text_obj) || !chat_obj) {
        return;
    }

    // Copy text to local buffer and remove bot name
    char text_buffer[TELEGRAM_MAX_MESSAGE_LENGTH_XS];
    strncpy(text_buffer, text_obj->valuestring, sizeof(text_buffer) - 1);
    text_buffer[sizeof(text_buffer) - 1] = '\0';
    strip_bot_name(text_buffer);  // Remove @BotName suffix

    const char *text = text_buffer;  // Use cleaned text
    // const char *text = text_obj->valuestring;

    cJSON *chat_id_obj = cJSON_GetObjectItem(chat_obj, "id");
    if (!chat_id_obj || !cJSON_IsNumber(chat_id_obj)) {
        return;
    }

    int64_t chat_id = (int64_t) chat_id_obj->valuedouble;

    last_chat_id = chat_id;  // Save last Chat ID

    ESP_LOGI(TAG, "Command from chat %" PRId64 ": %s", chat_id, text);

    char response_text[TELEGRAM_MAX_MESSAGE_LENGTH_L] = {0};
    cJSON *response_json = NULL;

    // === HELP ===
    if (strcmp(text, "/help") == 0 || strcmp(text, "/start") == 0) {
        snprintf(response_text, sizeof(response_text),
                 "📷 <b>PhotoFrame Bot Commands</b>\n\n"

                 "<b>📁 Albums</b>\n"
                 "• /albums — List all albums\n"
                 "• /images &lt;album&gt; — List images in album\n"
                 "• /create_album &lt;name&gt; — Create new album\n"
                 "• /delete_album &lt;name&gt; — Delete album\n"
                 "• /enable_album &lt;name&gt; — Enable album\n"
                 "• /disable_album &lt;name&gt; — Disable album\n\n"

                 "<b>🖼️ Display & Images</b>\n"
                 "• /display &lt;album/image&gt; — Show specific image\n"
                 "• /delete &lt;album/image&gt; — Delete image\n"
                 "• /txt &lt;text&gt; — Show text on next random image\n"
                 "• /history — View image history stats\n"
                 "• /clear_history — Reset displayed images\n\n"

                 "<b>🔄 Combine Mode (Portrait)</b>\n"
                 "• /combine — Show current status\n"
                 "• /combine &lt;on|off&gt; — Enable/disable combine mode\n\n"

                 "<b>⚙️ Configuration</b>\n"
                 "• /config — Show current config\n"
                 "• /set_interval &lt;seconds&gt; — Set rotation interval\n"
                 "• /auto_rotate &lt;on|off&gt; — Enable/disable auto-rotation\n"
                 "• /brightness &lt;-2.0 to 2.0&gt; — Adjust brightness (f-stops)\n"
                 "• /contrast &lt;0.5 to 2.0&gt; — Adjust contrast\n\n"

                 "<b>💬 Telegram Settings</b>\n"
                 "• /telegram_settings — Show Telegram settings\n"
                 "• /telegram_check_timer &lt;on|off&gt; — Check updates on auto-rotate\n"
                 "• /telegram_notify_timer &lt;on|off&gt; — Notify on auto-rotate\n"
                 "• /telegram_notify_sleep &lt;on|off&gt; — Notify before sleep\n\n"
				 "• /my_chat_id — Show chat ID\n\n"

                 "<b>🔋 System</b>\n"
                 "• /status — Show system status, IPs and memory\n"
                 "• /battery — Show battery status\n"
                 "• /sleep — Enter sleep mode\n\n"

                 "<b>📤 Upload (max. 2MB)</b>\n"
                 "Send a photo to add it to the Default album\n"
                 "📸 <b>Send Images</b>\n"
                 "• Send photos or files (JPEG)\n"
                 "• Add caption <code>#show</code> to display immediately\n"
                 "• Images are auto-processed\n\n");
    }

    // === ALBUMS ===
    else if (strcmp(text, "/albums") == 0) {
        if (api_get_albums(&response_json) == ESP_OK) {
            // strcpy(response_text, "<b>Albums:</b>\n");
            // snprintf(response_text, sizeof(response_text), "%s", "<b>Albums:</b>\n");
            int written = snprintf(response_text, sizeof(response_text), "<b>Albums:</b>\n");
            int count = cJSON_GetArraySize(response_json);
            for (int i = 0; i < count; i++) {
                cJSON *item = cJSON_GetArrayItem(response_json, i);
                const char *name = cJSON_GetObjectItem(item, "name")->valuestring;
                bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(item, "enabled"));
                char line[TELEGRAM_MAX_LINE_MESSAGE_LENGTH];
                snprintf(line, sizeof(line), "• %s %s\n", name, enabled ? "✅" : "❌");
                // strcat(response_text, line);
                if (written < sizeof(response_text)) {
                    written += snprintf(response_text + written, sizeof(response_text) - written,
                                        "%s", line);
                }
            }
        } else {
            // strcpy(response_text, "❌ Failed to get albums");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to get albums");
        }
    }

    // === IMAGES IN ALBUM ===
    else if (strncmp(text, "/images ", 8) == 0) {
        const char *album = text + 8;
        if (api_list_album_images(album, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "<b>Images in %s:</b>\n", album);
            int count = cJSON_GetArraySize(response_json);
            for (int i = 0; i < count && i < 20; i++) {
                cJSON *item = cJSON_GetArrayItem(response_json, i);
                const char *name = cJSON_GetObjectItem(item, "name")->valuestring;
                char line[TELEGRAM_MAX_LINE_MESSAGE_LENGTH];
                snprintf(line, sizeof(line), "• %s\n", name);
                strcat(response_text, line);
            }
            if (count == 0) {
                strcat(response_text, "(empty)");
            }
        } else {
            // strcpy(response_text, "❌ Album not found");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Album not found");
        }
    }

    // === CREATE ALBUM ===
    else if (strncmp(text, "/create_album ", 14) == 0) {
        const char *album = text + 14;
        if (api_create_album(album, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "✅ Album <b>%s</b> created", album);
        } else {
            // strcpy(response_text, "❌ Failed to create album");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to create album");
        }
    }

    // === DELETE ALBUM ===
    else if (strncmp(text, "/delete_album ", 14) == 0) {
        const char *album = text + 14;
        if (api_delete_album(album, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "✅ Album <b>%s</b> deleted", album);
        } else {
            // strcpy(response_text, "❌ Failed to delete album");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to delete album");
        }
    }

    // === ENABLE ALBUM ===
    else if (strncmp(text, "/enable_album ", 14) == 0) {
        const char *album = text + 14;
        if (api_set_album_enabled(album, true, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "✅ Album <b>%s</b> enabled", album);
        } else {
            // strcpy(response_text, "❌ Failed to enable album");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to enable album");
        }
    }

    // === DISABLE ALBUM ===
    else if (strncmp(text, "/disable_album ", 15) == 0) {
        const char *album = text + 15;
        if (api_set_album_enabled(album, false, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "✅ Album <b>%s</b> disabled", album);
        } else {
            // strcpy(response_text, "❌ Failed to disable album");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to disable album");
        }
    }

    // === DISPLAY IMAGE ===
    else if (strncmp(text, "/display ", 9) == 0) {
        const char *filename = text + 9;
        if (api_display_image(filename, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "✅ Displaying <b>%s</b>", filename);
            power_manager_reset_sleep_timer();
        } else {
            // strcpy(response_text, "❌ Display busy or image not found");
            snprintf(response_text, sizeof(response_text), "%s",
                     "❌ Display busy or image not found");
        }
    }

    // === DELETE IMAGE ===
    else if (strncmp(text, "/delete ", 8) == 0) {
        const char *filename = text + 8;
        if (api_delete_image(filename, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "✅ Deleted <b>%s</b>", filename);
        } else {
            // strcpy(response_text, "❌ Failed to delete image");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to delete image");
        }
    }

    // === GET CONFIG ===
    else if (strcmp(text, "/config") == 0) {
        if (api_get_config(&response_json) == ESP_OK) {
            int interval = cJSON_GetObjectItem(response_json, "rotate_interval")->valueint;
            bool auto_rotate = cJSON_IsTrue(cJSON_GetObjectItem(response_json, "auto_rotate"));
            double brightness = cJSON_GetObjectItem(response_json, "brightness_fstop")->valuedouble;
            double contrast = cJSON_GetObjectItem(response_json, "contrast")->valuedouble;

            snprintf(response_text, sizeof(response_text),
                     "<b>Current Configuration:</b>\n"
                     "Rotation Interval: %d sec\n"
                     "Auto-Rotate: %s\n"
                     "Brightness: %.1f f-stops\n"
                     "Contrast: %.1f",
                     interval, auto_rotate ? "ON" : "OFF", brightness, contrast);
        } else {
            // strcpy(response_text, "❌ Failed to get config");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to get config");
        }
    }

    // === SET INTERVAL ===
    else if (strncmp(text, "/set_interval ", 14) == 0) {
        int interval = atoi(text + 14);
        if (interval >= 10 && interval <= 86400) {
            cJSON *config = cJSON_CreateObject();
            cJSON_AddNumberToObject(config, "rotate_interval", interval);
            if (api_update_config(config, &response_json) == ESP_OK) {
                snprintf(response_text, sizeof(response_text), "✅ Interval set to %d seconds",
                         interval);
            } else {
                // strcpy(response_text, "❌ Failed to update interval");
                snprintf(response_text, sizeof(response_text), "%s",
                         "❌ Failed to update interval");
            }
            cJSON_Delete(config);
        } else {
            // strcpy(response_text, "❌ Invalid interval (10-86400 seconds)");
            snprintf(response_text, sizeof(response_text), "%s",
                     "❌ Invalid interval (10-86400 seconds)");
        }
    }

    // === AUTO ROTATE ===
    else if (strncmp(text, "/auto_rotate ", 13) == 0) {
        const char *param = text + 13;
        bool enable = (strcmp(param, "on") == 0 || strcmp(param, "1") == 0);
        cJSON *config = cJSON_CreateObject();
        cJSON_AddBoolToObject(config, "auto_rotate", enable);
        if (api_update_config(config, &response_json) == ESP_OK) {
            snprintf(response_text, sizeof(response_text), "✅ Auto-rotate %s",
                     enable ? "enabled" : "disabled");
        } else {
            // strcpy(response_text, "❌ Failed to update auto-rotate");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to update auto-rotate");
        }
        cJSON_Delete(config);
    }

    // === BRIGHTNESS ===
    else if (strncmp(text, "/brightness ", 12) == 0) {
        double brightness = atof(text + 12);
        if (brightness >= -2.0 && brightness <= 2.0) {
            cJSON *config = cJSON_CreateObject();
            cJSON_AddNumberToObject(config, "brightness_fstop", brightness);
            if (api_update_config(config, &response_json) == ESP_OK) {
                snprintf(response_text, sizeof(response_text), "✅ Brightness set to %.1f f-stops",
                         brightness);
            } else {
                // strcpy(response_text, "❌ Failed to update brightness");
                snprintf(response_text, sizeof(response_text), "%s",
                         "❌ Failed to update brightness");
            }
            cJSON_Delete(config);
        } else {
            // strcpy(response_text, "❌ Invalid brightness (-2.0 to 2.0)");
            snprintf(response_text, sizeof(response_text), "%s",
                     "❌ Invalid brightness (-2.0 to 2.0)");
        }
    }

    // === CONTRAST ===
    else if (strncmp(text, "/contrast ", 10) == 0) {
        double contrast = atof(text + 10);
        if (contrast >= 0.5 && contrast <= 2.0) {
            cJSON *config = cJSON_CreateObject();
            cJSON_AddNumberToObject(config, "contrast", contrast);
            if (api_update_config(config, &response_json) == ESP_OK) {
                snprintf(response_text, sizeof(response_text), "✅ Contrast set to %.1f", contrast);
            } else {
                // strcpy(response_text, "❌ Failed to update contrast");
                snprintf(response_text, sizeof(response_text), "%s",
                         "❌ Failed to update contrast");
            }
            cJSON_Delete(config);
        } else {
            // strcpy(response_text, "❌ Invalid contrast (0.5 to 2.0)");
            snprintf(response_text, sizeof(response_text), "%s",
                     "❌ Invalid contrast (0.5 to 2.0)");
        }
    }

    // === BATTERY ===
    else if (strcmp(text, "/battery") == 0) {
        if (api_get_battery(&response_json) == ESP_OK) {
            int voltage = cJSON_GetObjectItem(response_json, "battery_voltage_mv")->valueint;
            int percent = cJSON_GetObjectItem(response_json, "battery_percent")->valueint;
            bool charging = cJSON_IsTrue(cJSON_GetObjectItem(response_json, "charging"));
            bool usb = cJSON_IsTrue(cJSON_GetObjectItem(response_json, "usb_connected"));

            snprintf(response_text, sizeof(response_text),
                     "<b>🔋 Battery Status:</b>\n"
                     "Level: %d%% (%d mV)\n"
                     "Charging: %s\n"
                     "USB: %s",
                     percent, voltage, charging ? "Yes" : "No", usb ? "Connected" : "Disconnected");
        } else {
            // strcpy(response_text, "❌ Failed to get battery status");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to get battery status");
        }
    }

    // === SLEEP ===
    else if (strcmp(text, "/sleep") == 0) {
        // strcpy(response_text, "😴 Entering sleep mode...");
        snprintf(response_text, sizeof(response_text), "%s", "😴 Entering sleep mode...");
        telegram_bot_send_message(chat_id, response_text);
        telegram_polling_active = false;  // Stop polling
        sleep_requested = true;
        // vTaskDelay(pdMS_TO_TICKS(1000));
        // power_manager_enter_sleep();
        return;  // Won't reach here
    }

    // === GET MY CHAT ID ===
    else if (strcmp(text, "/my_chat_id") == 0 || strcmp(text, "/chatid") == 0) {
        snprintf(response_text, sizeof(response_text),
                 "📱 <b>Your Chat ID</b>\n\n"
                 "Chat ID: <code>%" PRId64
                 "</code>\n\n"
                 "ℹ️ Use this ID in the web interface to configure notifications.",
                 chat_id);
    }

    // === IMAGE HISTORY STATUS ===
    else if (strcmp(text, "/history") == 0) {
        int history_count = display_manager_get_history_count();

        snprintf(response_text, sizeof(response_text),
                 "📊 <b>Image History</b>\n\n"
                 "Images already displayed: <b>%d</b>\n\n"
                 "ℹ️ The system remembers which images were shown "
                 "and displays only unseen ones until all have been shown. "
                 "Then it automatically resets.\n\n"
                 "Use <code>/clear_history</code> to reset manually.",
                 history_count);
    }

    // === CLEAR IMAGE HISTORY ===
    else if (strcmp(text, "/clear_history") == 0) {
        ESP_LOGI(TAG, "Clearing image history...");

        int history_count = display_manager_get_history_count();
        display_manager_clear_history();

        snprintf(response_text, sizeof(response_text),
                 "🗑️ <b>Image History Cleared</b>\n\n"
                 "Removed <b>%d</b> images from history.\n\n"
                 "✅ All images will be shown again in random order.",
                 history_count);
    }

    // === DISPLAY WITH TEXT OVERLAY ===
    else if (strncmp(text, "/txt ", 5) == 0) {
        const char *overlay_text = text + 5;  // Text after "/txt "

        if (strlen(overlay_text) == 0) {
            strcpy(response_text,
                   "❌ <b>Usage</b>\n\n"
                   "<code>/txt Your text here</code>\n\n"
                   "Example:\n"
                   "<code>/txt Hello World!</code>");
        } else {
            ESP_LOGI(TAG, "Displaying next image with text overlay: %s", overlay_text);

            // Get random image from enabled albums
            char image_path[TELEGRAM_MAX_PATH_LENGTH] = {0};
            esp_err_t err = get_next_random_image(image_path, sizeof(image_path));

            if (err == ESP_OK) {
                // Display image with text overlay
                err = display_manager_show_image_with_text(image_path, overlay_text);

                if (err == ESP_OK) {
                    snprintf(response_text, sizeof(response_text),
                             "✅ <b>Image Updated with Text</b>\n\n"
                             "Text: \"%s\"\n\n"
                             "⏳ Display update takes ~30 seconds...",
                             overlay_text);
                } else {
                    // strcpy(response_text, "❌ Failed to update display");
                    snprintf(response_text, sizeof(response_text), "%s",
                             "❌ Failed to update display");
                }
            } else {
                // strcpy(response_text, "❌ No images available");
                snprintf(response_text, sizeof(response_text), "%s", "❌ No images available");
            }
        }
    }

    // === TELEGRAM SETTINGS ===
    else if (strcmp(text, "/telegram_settings") == 0) {
        bool check_on_timer = telegram_bot_get_check_on_timer_wakeup();
        bool notify_on_timer = telegram_bot_get_notify_on_timer_wakeup();
        bool notify_on_sleep = telegram_bot_get_notify_on_sleep();
        bool notify_on_display = telegram_bot_get_notify_on_display_update();

        snprintf(response_text, sizeof(response_text),
                 "📱 <b>Telegram Settings</b>\n\n"
                 "Check updates on timer wakeup: %s\n"
                 "Send notification on timer wakeup: %s\n"
                 "Send notification before sleep: %s\n"
                 "Send image notification on display update: %s\n\n"
                 "<b>Commands:</b>\n"
                 "<code>/telegram_check_timer [on|off]</code>\n"
                 "<code>/telegram_notify_timer [on|off]</code>\n"
                 "<code>/telegram_notify_sleep [on|off]</code>\n"
                 "<code>/telegram_notify_display [on|off]</code>",
                 check_on_timer ? "✅ ON" : "❌ OFF", notify_on_timer ? "✅ ON" : "❌ OFF",
                 notify_on_sleep ? "✅ ON" : "❌ OFF", notify_on_display ? "✅ ON" : "❌ OFF");
    }

    // === TELEGRAM NOTIFY DISPLAY ===
    else if (strncmp(text, "/telegram_notify_display ", 25) == 0) {
        const char *param = text + 25;
        bool enable = (strcmp(param, "on") == 0 || strcmp(param, "1") == 0);

        if (telegram_bot_set_notify_on_display_update(enable) == ESP_OK) {
            snprintf(response_text, sizeof(response_text),
                     "✅ Telegram notify on display update: <b>%s</b>\n\n"
                     "ℹ️ When enabled, you'll receive an image thumbnail notification "
                     "whenever the display is updated (including auto-rotate).",
                     enable ? "ENABLED" : "DISABLED");
        } else {
            // strcpy(response_text, "❌ Failed to update setting");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to update setting");
        }
    }

    // === TELEGRAM CHECK TIMER ===
    else if (strncmp(text, "/telegram_check_timer ", 22) == 0) {
        const char *param = text + 22;
        bool enable = (strcmp(param, "on") == 0 || strcmp(param, "1") == 0);

        if (telegram_bot_set_check_on_timer_wakeup(enable) == ESP_OK) {
            snprintf(response_text, sizeof(response_text),
                     "✅ Telegram check on timer wakeup: <b>%s</b>\n\n"
                     "ℹ️ When enabled, device will check for Telegram updates "
                     "during auto-rotate (uses more battery).",
                     enable ? "ENABLED" : "DISABLED");
        } else {
            // strcpy(response_text, "❌ Failed to update setting");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to update setting");
        }
    }

    // === TELEGRAM NOTIFY TIMER ===
    else if (strncmp(text, "/telegram_notify_timer ", 23) == 0) {
        const char *param = text + 23;
        bool enable = (strcmp(param, "on") == 0 || strcmp(param, "1") == 0);

        if (telegram_bot_set_notify_on_timer_wakeup(enable) == ESP_OK) {
            snprintf(response_text, sizeof(response_text),
                     "✅ Telegram notify on timer wakeup: <b>%s</b>\n\n"
                     "ℹ️ When enabled, you'll receive a notification with battery "
                     "status every time auto-rotate occurs (uses more battery).",
                     enable ? "ENABLED" : "DISABLED");
        } else {
            // strcpy(response_text, "❌ Failed to update setting");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to update setting");
        }
    }

    // === TELEGRAM NOTIFY SLEEP ===
    else if (strncmp(text, "/telegram_notify_sleep ", 23) == 0) {
        const char *param = text + 23;
        bool enable = (strcmp(param, "on") == 0 || strcmp(param, "1") == 0);

        if (telegram_bot_set_notify_on_sleep(enable) == ESP_OK) {
            snprintf(response_text, sizeof(response_text),
                     "✅ Telegram notify on sleep: <b>%s</b>\n\n"
                     "ℹ️ When enabled, you'll receive a notification with battery "
                     "status before device enters sleep mode.",
                     enable ? "ENABLED" : "DISABLED");
        } else {
            // strcpy(response_text, "❌ Failed to update setting");
            snprintf(response_text, sizeof(response_text), "%s", "❌ Failed to update setting");
        }
    }

    // Portrait Combine
    else if (strncmp(text, "/combine", 8) == 0) {
        handle_portrait_combine_command(text, chat_id);
    }
    // Status information
    else if (strncmp(text, "/status", 7) == 0) {
        handle_status_command(chat_id);
        return;
    }

    // Unknown command
    else {
        // strcpy(response_text, "❓ Unknown command. Send /help for available commands.");
        snprintf(response_text, sizeof(response_text), "%s",
                 "❓ Unknown command. Send /help for available commands.");
    }

    // Send response
    if (strlen(response_text) > 0) {
        telegram_bot_send_message(chat_id, response_text);
    }

    if (response_json) {
        cJSON_Delete(response_json);
    }
}

bool telegram_bot_check_updates(void)
{
    if (strlen(bot_token) == 0) {
        ESP_LOGW(TAG, "No bot token configured");
        return false;
    }
    // Updates einzeln verarbeiten mit limit=1
    char url[TELEGRAM_MAX_URL_LENGTH];
    snprintf(url, sizeof(url), "https://%s/bot%s/getUpdates?offset=%" PRId32 "&timeout=10&limit=1",
             TELEGRAM_API_BASE, bot_token, last_update_id + 1);

    ESP_LOGI(TAG, "Checking for Telegram updates (last ID: %" PRId32 ")", last_update_id);
    // ESP_LOGI(TAG, "Checking for updates from: %s", url);

    // Reset response buffer
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }
    http_response_len = 0;
    http_response_capacity = 0;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    // esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
        // return false;  // ← ÄNDERN von true zu false!
    }

    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status_code != 200) {
        ESP_LOGE(TAG, "getUpdates returned status %d", status_code);
        return ESP_FAIL;
    }

    // Parse response
    http_response_buffer[http_response_len] = '\0';
    cJSON *response = cJSON_Parse(http_response_buffer);
    if (!response) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }

    cJSON *ok = cJSON_GetObjectItem(response, "ok");
    if (!ok || !cJSON_IsTrue(ok)) {
        ESP_LOGE(TAG, "API returned ok=false");
        cJSON_Delete(response);
        if (http_response_buffer) {
            free(http_response_buffer);
            http_response_buffer = NULL;
        }
        return ESP_FAIL;
    }

    bool had_updates = false;

    // Process updates
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || !cJSON_IsArray(result)) {
        ESP_LOGW(TAG, "No result array in response");
        cJSON_Delete(response);
        if (http_response_buffer) {
            free(http_response_buffer);
            http_response_buffer = NULL;
        }
        return false;
    }

    int update_count = cJSON_GetArraySize(result);
    ESP_LOGI(TAG, "Received %d updates", update_count);

    if (update_count > 0) {
        had_updates = true;
    }

    // Process each update
    for (int i = 0; i < update_count; i++) {
        cJSON *update = cJSON_GetArrayItem(result, i);
        cJSON *update_id_obj = cJSON_GetObjectItem(update, "update_id");

        if (!update_id_obj || !cJSON_IsNumber(update_id_obj)) {
            ESP_LOGW(TAG, "Update without valid update_id");
            continue;
        }

        // int32_t current_update_id = (int32_t)update_id_obj->valuedouble;
        int32_t current_update_id;
        if (cJSON_IsNumber(update_id_obj)) {
            // Telegram update_id ist immer positiv und < 2^31
            current_update_id = (int32_t) update_id_obj->valueint;

            // Fallback for big numbers
            if (update_id_obj->valuedouble > INT32_MAX || update_id_obj->valuedouble < 0) {
                ESP_LOGW(TAG, "Invalid update_id: %.0f, skipping", update_id_obj->valuedouble);
                continue;
            }
        } else {
            ESP_LOGW(TAG, "Update_id is not a number");
            continue;
        }

        // IMPORTANT: Update the update ID BEFORE we process the update.
        if (current_update_id > last_update_id) {
            last_update_id = current_update_id;
            ESP_LOGI(TAG, "Processing update_id: %" PRId32, last_update_id);
        } else {
            ESP_LOGW(TAG, "Skipping old/duplicate update_id: %" PRId32 " (last: %" PRId32 ")",
                     current_update_id, last_update_id);
            continue;  // Skip already processed updates
        }

        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) {
            ESP_LOGW(TAG, "Update has no message");
            continue;
        }

        // Check if it's a photo message
        cJSON *photo = cJSON_GetObjectItem(message, "photo");
        if (photo && cJSON_IsArray(photo)) {
            ESP_LOGI(TAG, "Received photo message");

            // Get chat_id for confirmation message
            cJSON *chat_obj = cJSON_GetObjectItem(message, "chat");
            int64_t chat_id = 0;
            if (chat_obj) {
                cJSON *chat_id_obj = cJSON_GetObjectItem(chat_obj, "id");
                if (chat_id_obj && cJSON_IsNumber(chat_id_obj)) {
                    chat_id = (int64_t) chat_id_obj->valuedouble;
                }
            }

            esp_err_t process_err = process_photo_message(message);

            if (process_err == ESP_ERR_INVALID_STATE) {
                // Duplicate detected - notification already sent in process_photo_message()
                ESP_LOGI(TAG, "Duplicate photo detected, skipping success message");
            } else if (process_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to process photo: %s", esp_err_to_name(process_err));
                if (chat_id != 0) {
                    telegram_bot_send_message(chat_id,
                                              "❌ <b>Error</b>\n\n"
                                              "Failed to process image. Please try:\n"
                                              "• A smaller image (max 2 MB)\n"
                                              "• JPEG format only\n"
                                              "• Non-progressive JPEG");
                }
            } else {
                ESP_LOGI(TAG, " Photo processed successfully");
                if (chat_id != 0) {
                    // Check if image was displayed
                    cJSON *caption_obj = cJSON_GetObjectItem(message, "caption");
                    const char *caption = NULL;
                    if (caption_obj && cJSON_IsString(caption_obj)) {
                        caption = caption_obj->valuestring;
                    }

                    if (caption && should_display_immediately(caption)) {
                        telegram_bot_send_message(chat_id,
                                                  "✅ <b>Image Received & Displayed!</b>\n\n"
                                                  "📺 The image is now showing on your display.");
                    } else {
                        telegram_bot_send_message(
                            chat_id,
                            "✅ <b>Image Received!</b>\n\n"
                            "Image successfully processed and added to your album.\n"
                            "It will appear in your photo rotation.\n"
                            "💡 <i>Tip: Use #show in caption to display immediately</i>");
                    }
                }
            }
            continue;
        }

        // Check if it's a document message (image sent as file)
        cJSON *document = cJSON_GetObjectItem(message, "document");
        if (document && cJSON_IsObject(document)) {
            // Get chat_id for messages
            cJSON *chat_obj = cJSON_GetObjectItem(message, "chat");
            int64_t chat_id = 0;
            if (chat_obj) {
                cJSON *chat_id_obj = cJSON_GetObjectItem(chat_obj, "id");
                if (chat_id_obj && cJSON_IsNumber(chat_id_obj)) {
                    chat_id = (int64_t) chat_id_obj->valuedouble;
                }
            }

            // Check if it's an image file by mime_type
            cJSON *mime_type_obj = cJSON_GetObjectItem(document, "mime_type");
            if (mime_type_obj && cJSON_IsString(mime_type_obj)) {
                const char *mime_type = mime_type_obj->valuestring;

                // Check if it's a JPEG image
                if (strstr(mime_type, "image/jpeg") || strstr(mime_type, "image/jpg")) {
                    ESP_LOGI(TAG, "Received image as document (JPEG)");

                    // Process document as photo
                    esp_err_t process_err = process_document_as_photo(message);

                    if (process_err == ESP_ERR_INVALID_STATE) {
                        // Duplicate detected - notification already sent in
                        // process_document_as_photo()
                        ESP_LOGI(TAG, "Duplicate document detected, skipping success message");
                    } else if (process_err != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to process document: %s",
                                 esp_err_to_name(process_err));
                        if (chat_id != 0) {
                            telegram_bot_send_message(chat_id,
                                                      "❌ <b>Error</b>\n\n"
                                                      "Failed to process document. Please ensure "
                                                      "it's a valid JPEG image.");
                        }
                    } else {
                        ESP_LOGI(TAG, " Document processed successfully");
                        if (chat_id != 0) {
                            // Check if image was displayed
                            cJSON *caption_obj = cJSON_GetObjectItem(message, "caption");
                            const char *caption = NULL;
                            if (caption_obj && cJSON_IsString(caption_obj)) {
                                caption = caption_obj->valuestring;
                            }

                            if (caption && should_display_immediately(caption)) {
                                telegram_bot_send_message(
                                    chat_id,
                                    "✅ <b>Image Received & Displayed!</b>\n\n"
                                    "📺 The image is now showing on your display.");
                            } else {
                                telegram_bot_send_message(
                                    chat_id,
                                    "✅ <b>Image Received!</b>\n\n"
                                    "Image successfully processed and added to your album.\n\n"
                                    "It will appear in your photo rotation.\n\n"
                                    "💡 <i>Tip: Use #show to display immediately</i>");
                            }
                        }
                    }

                    continue;
                } else {
                    ESP_LOGW(TAG, "Document is not a JPEG image: %s", mime_type);
                    if (chat_id != 0) {
                        telegram_bot_send_message(chat_id,
                                                  "⚠️ <b>Unsupported Format</b>\n\n"
                                                  "Please send images as JPEG format only.");
                    }
                    continue;
                }
            }
        }

        // Check if it's a command
        cJSON *text = cJSON_GetObjectItem(message, "text");
        if (text && cJSON_IsString(text)) {
            const char *text_str = text->valuestring;
            if (text_str[0] == '/') {
                ESP_LOGI(TAG, "Received command: %s", text_str);
                handle_telegram_command(message);
            } else {
                ESP_LOGI(TAG, "Received text message (not a command)");
            }
        }
    }

    // Save last_update_id to NVS after processing
    if (had_updates) {
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_i32(nvs_handle, NVS_TELEGRAM_UPDATEID_KEY, last_update_id);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Saved last_update_id to NVS: %" PRId32, last_update_id);
        }
    }

    cJSON_Delete(response);
    if (http_response_buffer) {
        free(http_response_buffer);
        http_response_buffer = NULL;
    }

    if (sleep_requested) {
        ESP_LOGI(TAG, "Sleep requested, entering deep sleep in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        power_manager_enter_sleep();
        // Wird nie erreicht
    }

    ESP_LOGI(TAG, "Update check complete (last_update_id: %" PRId32 ")", last_update_id);
    return had_updates;
}

void telegram_bot_stop_polling(void)
{
    extern bool telegram_polling_active;
    telegram_polling_active = false;
    ESP_LOGI(TAG, "Telegram polling stopped");
}

/**
 * @brief Check if a photo send operation is in progress
 * Thread-safe check using mutex
 */
bool telegram_bot_is_sending_photo(void)
{
    if (!photo_send_mutex) {
        return false;
    }

    bool sending = false;
    if (xSemaphoreTake(photo_send_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sending = photo_send_in_progress;
        xSemaphoreGive(photo_send_mutex);
    }

    return sending;
}
