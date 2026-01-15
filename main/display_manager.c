#include "display_manager.h"

#include <dirent.h>
#include <stdio.h>  // <-- v1.9.0_tlg Telegram
#include <string.h>
#include <sys/stat.h>

#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "GUI_Paint.h"  // <-- v1.9.0_tlg Telegram
#include "album_manager.h"
#include "axp_prot.h"  // <-- v1.9.0_tlg Telegram For axp_get_battery_percent(), axp_is_usb_connected()
#include "config.h"
#include "config_manager.h"
#include "epaper_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"       // <-- v1.9.0_tlg Telegram
#include "freertos/FreeRTOS.h"  // <-- v1.9.0_tlg Telegram
#include "freertos/semphr.h"    // <-- v1.9.0_tlg Telegram
#include "main.h"               // <-- v1.9.0_tlg Telegram Function to set display update flag
#include "mbedtls/md5.h"        // <-- v1.9.0_tlg Telegram
#include "nvs.h"                // <-- v1.9.0_tlg Telegram
#include "nvs_flash.h"          // <-- v1.9.0_tlg Telegram
#include "power_manager.h"      // <-- v1.9.0_tlg Telegram
#include "telegram_bot.h"       // <-- v1.9.0_tlg Telegram
#include "wifi_manager.h"       // <-- v1.9.0_tlg Telegram

static const char *TAG = "display_manager";

// v1.9.0_tlg Telegram START
// ToDo: Check clearing of allocated storage space
// NVS Keys for Image History
#define NVS_IMAGE_HISTORY_KEY "img_history"
#define NVS_IMAGE_HISTORY_COUNT_KEY "img_hist_cnt"
#define MAX_IMAGE_HISTORY 1000  // max 1000 images (16 KB NVS)
// v1.9.0_tlg Telegram END

static SemaphoreHandle_t display_mutex = NULL;           // <-- v1.9.0_tlg Telegram
static int rotate_interval = IMAGE_ROTATE_INTERVAL_SEC;  // <-- v1.9.0_tlg Telegram
static bool auto_rotate_enabled = false;                 // <-- v1.9.0_tlg Telegram
static char current_image[64] = {0};

static uint8_t *epd_image_buffer = NULL;
static uint32_t image_buffer_size;

// v1.9.0_tlg Telegram START
// Save the complete path of the currently displayed image
static char current_image_path[TELEGRAM_MAX_PATH_LENGTH] = {0};

// Image History - List of images already displayed (as MD5 hashes)
static uint8_t (*displayed_image_hashes)[16] = NULL;  // MD5 = 16 bytes per image
static int displayed_count = 0;

// === IMAGE HISTORY MANAGEMENT ===
// (Forward declaration - optional)
// === FORWARD DECLARATIONS ===

// Image History Management
static void calculate_image_hash(const char *path, uint8_t hash[16]);
static void load_image_history(void);
static void save_image_history(void);
static void clear_image_history(void);
static bool is_image_displayed(const char *image_path);
static void add_to_history(const char *image_path);

// Text Overlay Helpers
static void sanitize_overlay_text(const char *input, char *output, size_t output_size,
                                  int max_length);
// static void utf8_to_cp1252(const char *utf8_text, char *output, size_t output_size);
static void wrap_text_smart(const char *text, char *line1, char *line2, size_t line_size,
                            int max_chars_per_line);
static void draw_text_overlay(const char *text);
static esp_err_t load_overlay_text(const char *bmp_path, char *text_buffer, size_t buffer_size);

// ? NEW: Battery Warning Overlay
static const char *get_battery_warning_text(void);
static void draw_battery_warning_overlay(void);
// === END FORWARD DECLARATIONS ===

/**
 * @brief Get the next image that would be displayed (without displaying it)
 * @param imagepath Buffer to store the image path
 * @param pathsize Size of the buffer
 * @return ESP_OK on success
 */
esp_err_t display_manager_get_next_image(char *imagepath, size_t pathsize)
{
    if (!imagepath || pathsize == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!auto_rotate_enabled) {
        ESP_LOGI(TAG, "Auto-rotate disabled");
        return ESP_FAIL;
    }

    // Get enabled albums
    char **enabled_albums = NULL;
    int album_count = 0;
    if (album_manager_get_enabled_albums(&enabled_albums, &album_count) != ESP_OK ||
        album_count == 0) {
        ESP_LOGW(TAG, "No enabled albums");
        return ESP_FAIL;
    }

    // Build image list (GLEICHER Code wie in display_manager_handle_wakeup)
    int total_image_count = 0;
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
                    total_image_count++;
                }
            }
        }
        closedir(dir);
    }

    if (total_image_count == 0) {
        album_manager_free_album_list(enabled_albums, album_count);
        return ESP_FAIL;
    }

    // Build full image list
    char **image_list = malloc(total_image_count * sizeof(char *));
    if (!image_list) {
        album_manager_free_album_list(enabled_albums, album_count);
        return ESP_ERR_NO_MEM;
    }

    int idx = 0;
    for (int i = 0; i < album_count; i++) {
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));

        DIR *dir = opendir(album_path);
        if (!dir)
            continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && idx < total_image_count) {
            if (entry->d_type == DT_REG) {
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                    char *fullpath = malloc(512);
                    if (fullpath) {
                        snprintf(fullpath, 512, "%s/%s", album_path, entry->d_name);
                        image_list[idx++] = fullpath;
                    }
                }
            }
        }
        closedir(dir);
    }
    album_manager_free_album_list(enabled_albums, album_count);

    // Filter undisplayed images
    char **undisplayed_list = malloc(total_image_count * sizeof(char *));
    if (!undisplayed_list) {
        for (int i = 0; i < total_image_count; i++)
            free(image_list[i]);
        free(image_list);
        return ESP_ERR_NO_MEM;
    }

    int undisplayed_count = 0;
    for (int i = 0; i < total_image_count; i++) {
        if (!is_image_displayed(image_list[i])) {
            undisplayed_list[undisplayed_count++] = image_list[i];
        }
    }

    // If all displayed, use all images
    if (undisplayed_count == 0) {
        for (int i = 0; i < total_image_count; i++) {
            undisplayed_list[i] = image_list[i];
        }
        undisplayed_count = total_image_count;
    }

    // Select random image
    int random_index = esp_random() % undisplayed_count;
    strncpy(imagepath, undisplayed_list[random_index], pathsize - 1);
    imagepath[pathsize - 1] = '\0';

    // Cleanup
    free(undisplayed_list);
    for (int i = 0; i < total_image_count; i++)
        free(image_list[i]);
    free(image_list);

    return ESP_OK;
}

// Calculate MD5 hash of an image path
static void calculate_image_hash(const char *path, uint8_t hash[16])
{
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const unsigned char *) path, strlen(path));
    mbedtls_md5_finish(&ctx, hash);
    mbedtls_md5_free(&ctx);
}

// Load displayed images from NVS
static void load_image_history(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        ESP_LOGD(TAG, "Failed to open NVS for image history");
        return;
    }

    // Get count
    int32_t count = 0;
    if (nvs_get_i32(nvs_handle, NVS_IMAGE_HISTORY_COUNT_KEY, &count) != ESP_OK || count <= 0) {
        nvs_close(nvs_handle);
        return;
    }

    if (count > MAX_IMAGE_HISTORY) {
        ESP_LOGW(TAG, "History count %ld exceeds max %d, limiting", count, MAX_IMAGE_HISTORY);
        count = MAX_IMAGE_HISTORY;
    }

    ESP_LOGI(TAG, "Loading %ld image hashes from history", count);

    // Allocate memory for hashes
    displayed_image_hashes = heap_caps_malloc(MAX_IMAGE_HISTORY * 16, MALLOC_CAP_8BIT);
    if (!displayed_image_hashes) {
        ESP_LOGE(TAG, "Failed to allocate memory for image history");
        nvs_close(nvs_handle);
        return;
    }
    memset(displayed_image_hashes, 0, MAX_IMAGE_HISTORY * 16);

    // Load hash blob
    size_t blob_size = count * 16;
    esp_err_t err =
        nvs_get_blob(nvs_handle, NVS_IMAGE_HISTORY_KEY, displayed_image_hashes, &blob_size);
    if (err == ESP_OK) {
        displayed_count = blob_size / 16;  // Calculate actual count from blob size
        ESP_LOGI(TAG, "? Loaded %d image hashes from history", displayed_count);
    } else {
        ESP_LOGW(TAG, "Failed to load history blob: %s", esp_err_to_name(err));
        free(displayed_image_hashes);
        displayed_image_hashes = NULL;
    }

    nvs_close(nvs_handle);
}

// Save displayed images to NVS
static void save_image_history(void)
{
    if (!displayed_image_hashes || displayed_count == 0) {
        return;
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS to save image history");
        return;
    }

    // Save count
    esp_err_t err = nvs_set_i32(nvs_handle, NVS_IMAGE_HISTORY_COUNT_KEY, displayed_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save history count: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return;
    }

    // Save hash blob (much more efficient than individual keys!)
    err = nvs_set_blob(nvs_handle, NVS_IMAGE_HISTORY_KEY, displayed_image_hashes,
                       displayed_count * 16);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save history blob: %s", esp_err_to_name(err));
    }

    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGD(TAG, "Saved %d image hashes to history", displayed_count);
}

// Clear image history
static void clear_image_history(void)
{
    ESP_LOGI(TAG, "Clearing image history (%d entries)", displayed_count);

    // Free memory
    if (displayed_image_hashes) {
        free(displayed_image_hashes);
        displayed_image_hashes = NULL;
    }
    displayed_count = 0;

    // Clear NVS
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_key(nvs_handle, NVS_IMAGE_HISTORY_COUNT_KEY);
        nvs_erase_key(nvs_handle, NVS_IMAGE_HISTORY_KEY);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "? Image history cleared");
}

// Check if image was already displayed
static bool is_image_displayed(const char *image_path)
{
    // ESP_LOGI(TAG, "  ? is_image_displayed() checking: %s", image_path);

    /*
if (!displayed_image_hashes || displayed_count == 0) {
    ESP_LOGI(TAG, "  ? History empty, returning FALSE");
    return false;
}
    */

    uint8_t hash[16];
    calculate_image_hash(image_path, hash);

    // ESP_LOGI(TAG, "  ? Comparing against %d entries in history", displayed_count);

    for (int i = 0; i < displayed_count; i++) {
        if (memcmp(displayed_image_hashes[i], hash, 16) == 0) {
            ESP_LOGW(TAG, "  ? MATCH at index %d, returning TRUE", i);
            return true;
        }
    }

    // ESP_LOGI(TAG, "  ? No match found, returning FALSE");
    return false;
}

/*
static bool is_image_displayed(const char *image_path) {
    if (!displayed_image_hashes || displayed_count == 0) {
        return false;
    }

    uint8_t hash[16];
    calculate_image_hash(image_path, hash);

    for (int i = 0; i < displayed_count; i++) {
        if (memcmp(displayed_image_hashes[i], hash, 16) == 0) {
            return true;
        }
    }
    return false;
}
*/

// Add image to history
static void add_to_history(const char *image_path)
{
    ESP_LOGI(TAG, "=== ADD_TO_HISTORY START ===");
    ESP_LOGI(TAG, "Path: %s", image_path);
    ESP_LOGI(TAG, "Current history count: %d", displayed_count);

    // Calculate hash for debugging
    uint8_t test_hash[16];
    calculate_image_hash(image_path, test_hash);
    ESP_LOGI(TAG, "Hash: %02x%02x%02x%02x...", test_hash[0], test_hash[1], test_hash[2],
             test_hash[3]);

    // Check if already in history
    if (is_image_displayed(image_path)) {
        ESP_LOGW(TAG, "? Image already in history, skipping!");
        ESP_LOGI(TAG, "=== ADD_TO_HISTORY END (skipped) ===");
        return;
    }

    // Allocate memory if needed
    if (!displayed_image_hashes) {
        ESP_LOGI(TAG, "Allocating new history buffer");
        displayed_image_hashes = heap_caps_malloc(MAX_IMAGE_HISTORY * 16, MALLOC_CAP_8BIT);
        if (!displayed_image_hashes) {
            ESP_LOGE(TAG, "Failed to allocate memory for image history");
            return;
        }
        memset(displayed_image_hashes, 0, MAX_IMAGE_HISTORY * 16);
        displayed_count = 0;
    }

    // Calculate and add hash
    calculate_image_hash(image_path, displayed_image_hashes[displayed_count]);
    displayed_count++;

    ESP_LOGI(TAG, "? Successfully added to history!");
    ESP_LOGI(TAG, "New history count: %d/%d", displayed_count, MAX_IMAGE_HISTORY);
    ESP_LOGI(TAG, "=== ADD_TO_HISTORY END ===");

    // Save to NVS every 10 images to reduce flash wear
    // Save to NVS - always save for first 10 images, then every 10th
    // if (displayed_count <= 10 || (displayed_count % 10 == 0)) {
    if ((displayed_count % 10 == 0) || displayed_count == 1) {
        save_image_history();
        ESP_LOGI(TAG, "Saved history to NVS");
    }
}

// === END IMAGE HISTORY ===
// v1.9.0_tlg Telegram END

esp_err_t display_manager_init(void)
{
    display_mutex = xSemaphoreCreateMutex();
    if (!display_mutex) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        return ESP_FAIL;
    }

    epaper_port_init();

    image_buffer_size =
        ((DISPLAY_WIDTH % 2 == 0) ? (DISPLAY_WIDTH / 2) : (DISPLAY_WIDTH / 2 + 1)) * DISPLAY_HEIGHT;
    epd_image_buffer = (uint8_t *) heap_caps_malloc(image_buffer_size, MALLOC_CAP_SPIRAM);
    if (!epd_image_buffer) {
        ESP_LOGE(TAG, "Failed to allocate image buffer");
        return ESP_FAIL;
    }

    Paint_NewImage(epd_image_buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, EPD_7IN3E_WHITE);
    Paint_SetScale(6);
    Paint_SelectImage(epd_image_buffer);
    Paint_SetRotate(180);

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        int32_t stored_interval = IMAGE_ROTATE_INTERVAL_SEC;
        if (nvs_get_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, &stored_interval) == ESP_OK) {
            rotate_interval = stored_interval;
            ESP_LOGI(TAG, "Loaded rotate interval from NVS: %d seconds", rotate_interval);
        }

        uint8_t stored_enabled = 0;
        if (nvs_get_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, &stored_enabled) == ESP_OK) {
            auto_rotate_enabled = (stored_enabled != 0);
            ESP_LOGI(TAG, "Loaded auto-rotate enabled from NVS: %s",
                     auto_rotate_enabled ? "yes" : "no");
        }

        nvs_close(nvs_handle);
    }

    // v1.9.0_tlg Telegram: Load image history
    load_image_history();

    ESP_LOGI(TAG, "Display manager initialized");
    ESP_LOGI(TAG, "Auto-rotate uses timer-based wake-up (only works during sleep cycles)");
    return ESP_OK;
}

esp_err_t display_manager_show_image(const char *filename)
{
    if (!filename || strlen(filename) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_FAIL;
    }

    // Expect absolute path from caller
    ESP_LOGI(TAG, "Displaying image: %s", filename);
    ESP_LOGI(TAG, "Free heap before display: %lu bytes", esp_get_free_heap_size());

    // -------------------------------------------------------------------
    // v1.9.0_tlg Telegram: Try to load Text-Overlay from .txt file
    // -------------------------------------------------------------------
    char overlay_text[TELEGRAM_MAX_MESSAGE_LENGTH_XS] = {0};
    bool has_overlay = (load_overlay_text(filename, overlay_text, sizeof(overlay_text)) == ESP_OK);

    ESP_LOGI(TAG, "Clearing display buffer");
    Paint_Clear(EPD_7IN3E_WHITE);

    ESP_LOGI(TAG, "Reading BMP file into buffer");
    if (GUI_ReadBmp_RGB_6Color(filename, 0, 0) != 0) {
        ESP_LOGE(TAG, "Failed to read BMP file");
        xSemaphoreGive(display_mutex);
        return ESP_FAIL;
    }

    // -------------------------------------------------------------------
    // v1.9.0_tlg Telegram: Draw Text-Overlay if exist
    // -------------------------------------------------------------------
    if (has_overlay) {
        ESP_LOGI(TAG, "Drawing overlay text: %s", overlay_text);
        draw_text_overlay(overlay_text);
    }

    // -------------------------------------------------------------------
    // ? NEW: Draw battery warning overlay if needed
    // -------------------------------------------------------------------
    draw_battery_warning_overlay();

    ESP_LOGI(TAG, "Starting e-paper display update (this takes ~30 seconds)");
    ESP_LOGI(TAG, "Free heap before epaper_port_display: %lu bytes", esp_get_free_heap_size());

    // Yield to watchdog before long operation
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Calling epaper_port_display...");
    epaper_port_display(epd_image_buffer);
    ESP_LOGI(TAG, "epaper_port_display returned successfully");

    ESP_LOGI(TAG, "E-paper display update complete");
    ESP_LOGI(TAG, "Free heap after display: %lu bytes", esp_get_free_heap_size());

    strncpy(current_image, filename, sizeof(current_image) - 1);

    // v1.9.0_tlg Telegram: Save path
    strncpy(current_image_path, filename, sizeof(current_image_path) - 1);
    current_image_path[sizeof(current_image_path) - 1] = '\0';

    xSemaphoreGive(display_mutex);

    // v1.9.0_tlg Telegram: Add to History (after mutex release)
    add_to_history(filename);

    ESP_LOGI(TAG, "Image displayed successfully");
    main_set_display_updated_by_telegram(true);

    return ESP_OK;
}

// v1.9.0_tlg Telegram: Getter function
esp_err_t display_manager_get_current_image(char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(current_image_path) == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(buffer, current_image_path, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    return ESP_OK;
}

esp_err_t display_manager_clear(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_FAIL;
    }

    epaper_port_clear(epd_image_buffer, EPD_7IN3E_WHITE);
    epaper_port_display(epd_image_buffer);

    xSemaphoreGive(display_mutex);
    return ESP_OK;
}

bool display_manager_is_busy(void)
{
    // Try to take the mutex without blocking
    if (xSemaphoreTake(display_mutex, 0) == pdTRUE) {
        // Mutex was available, give it back
        xSemaphoreGive(display_mutex);
        return false;
    }
    // Mutex is held by another task
    return true;
}

void display_manager_set_rotate_interval(int seconds)
{
    rotate_interval = seconds;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, seconds);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Rotate interval set to %d seconds", seconds);
}

int display_manager_get_rotate_interval(void)
{
    return rotate_interval;
}

void display_manager_set_auto_rotate(bool enabled)
{
    auto_rotate_enabled = enabled;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Auto-rotate %s", enabled ? "enabled" : "disabled");
}

bool display_manager_get_auto_rotate(void)
{
    return auto_rotate_enabled;
}

void display_manager_handle_wakeup(void)
{
    if (!config_manager_get_auto_rotate()) {  // if (!auto_rotate_enabled) {
        ESP_LOGI(TAG, "Manual rotation triggered (auto-rotate is disabled)");
    } else {
        ESP_LOGI(TAG, "Handling wakeup for auto-rotate");
    }

    // Get enabled albums
    char **enabled_albums = NULL;
    int album_count = 0;
    if (album_manager_get_enabled_albums(&enabled_albums, &album_count) != ESP_OK ||
        album_count == 0) {
        ESP_LOGW(TAG, "No enabled albums for auto-rotate");
        return;
    }

    ESP_LOGI(TAG, "Collecting images from %d enabled album(s)", album_count);

    // Check for stale albums (removed from SD card) and disable them
    bool found_stale_albums = false;
    for (int i = 0; i < album_count; i++) {
        if (!album_manager_album_exists(enabled_albums[i])) {
            ESP_LOGW(TAG, "Album '%s' no longer exists on SD card, disabling it",
                     enabled_albums[i]);
            album_manager_set_album_enabled(enabled_albums[i], false);
            found_stale_albums = true;
        }
    }

    // If we found stale albums, reload the enabled list
    if (found_stale_albums) {
        album_manager_free_album_list(enabled_albums, album_count);
        if (album_manager_get_enabled_albums(&enabled_albums, &album_count) != ESP_OK ||
            album_count == 0) {
            ESP_LOGW(TAG, "No enabled albums remaining after cleanup");
            return;
        }
        ESP_LOGI(TAG, "After cleanup: %d enabled album(s)", album_count);
    }

    // Count total images across all enabled albums
    int total_image_count = 0;
    for (int i = 0; i < album_count; i++) {
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];  // char album_path[256];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));

        DIR *dir = opendir(album_path);
        if (!dir) {
            ESP_LOGW(TAG, "Failed to open album: %s", enabled_albums[i]);
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                    continue;
                }
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                    total_image_count++;
                }
            }
        }
        closedir(dir);
    }

    if (total_image_count == 0) {
        ESP_LOGW(TAG, "No images found in enabled albums");
        album_manager_free_album_list(enabled_albums, album_count);
        return;
    }
    // v1.9.0_tlg Telegram
    ESP_LOGI(TAG, "Found %d total images in enabled albums", total_image_count);

    // Build image list with absolute paths from all enabled albums
    char **image_list = malloc(total_image_count * sizeof(char *));
    // v1.9.0_tlg Telegram:
    if (!image_list) {
        ESP_LOGE(TAG, "Failed to allocate memory for image list");
        album_manager_free_album_list(enabled_albums, album_count);
        return;
    }

    int idx = 0;

    for (int i = 0; i < album_count; i++) {
        char album_path[TELEGRAM_MAX_ALBUM_PATH_LENGTH];  // char album_path[256];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));

        DIR *dir = opendir(album_path);
        if (!dir) {
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && idx < total_image_count) {
            if (entry->d_type == DT_REG) {
                if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                    continue;
                }

                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                    char *fullpath = malloc(512);
                    // v1.9.0_tlg Telegram:
                    if (fullpath) {
                        snprintf(fullpath, 512, "%s/%s", album_path, entry->d_name);
                        image_list[idx] = fullpath;
                        idx++;
                    }
                }
            }
        }
        closedir(dir);
    }

    album_manager_free_album_list(enabled_albums, album_count);

    // === v1.9.0_tlg Telegram: Filter undisplayed images ===
    char **undisplayed_list = malloc(total_image_count * sizeof(char *));
    if (!undisplayed_list) {
        ESP_LOGE(TAG, "Failed to allocate memory for undisplayed list");
        for (int i = 0; i < total_image_count; i++) {
            free(image_list[i]);
        }
        free(image_list);
        return;
    }

    int undisplayed_count = 0;
    for (int i = 0; i < total_image_count; i++) {
        if (!is_image_displayed(image_list[i])) {
            undisplayed_list[undisplayed_count++] = image_list[i];
        }
    }

    ESP_LOGI(TAG, "Images: %d total, %d not yet displayed, %d in history", total_image_count,
             undisplayed_count, displayed_count);

    // If all images have been displayed, reset history and start fresh
    if (undisplayed_count == 0) {
        ESP_LOGI(TAG, "?? All %d images have been displayed! Resetting history for new cycle...",
                 total_image_count);

        // Send Telegram notification BEFORE clearing
        if (wifi_manager_is_connected()) {
            int64_t chat_id = telegram_bot_get_last_chat_id();
            if (chat_id != 0 && telegram_bot_has_token()) {
                char message[256];
                snprintf(message, sizeof(message),
                         "<b>?? Image History Reset</b>\n"
                         "All <b>%d</b> images have been displayed.\n"
                         "Starting new cycle...",
                         total_image_count);
                telegram_bot_send_message(chat_id, message);
                ESP_LOGI(TAG, "History reset notification sent");
            }
        }

        clear_image_history();

        // Use all images again
        for (int i = 0; i < total_image_count; i++) {
            undisplayed_list[i] = image_list[i];
        }
        undisplayed_count = total_image_count;
    }

    // Select random undisplayed image
    int random_index = esp_random() % undisplayed_count;
    char *selected_image = undisplayed_list[random_index];

    // Display random image
    // ESP_LOGI(TAG, "Auto-rotate: Displaying random image %d/%d: %s", random_index + 1,
    // total_image_count, image_list[random_index]);
    // display_manager_show_image(image_list[random_index]);

    ESP_LOGI(TAG, "Auto-rotate: Displaying random undisplayed image %d/%d: %s", random_index + 1,
             undisplayed_count, selected_image);

    display_manager_show_image(selected_image);

    // Free image list
    free(undisplayed_list);

    for (int i = 0; i < total_image_count; i++) {
        free(image_list[i]);
    }
    free(image_list);

    ESP_LOGI(TAG, "Auto-rotate complete");
}

// v1.9.0_tlg Telegram: API for manual history management
int display_manager_get_history_count(void)
{
    return displayed_count;
}

void display_manager_clear_history(void)
{
    clear_image_history();
}

// v1.9.0_tlg Telegram: Convert UTF-8 to ISO-8859-1/CP1252 for e-paper display
/*
static void utf8_to_cp1252(const char *utf8_text, char *output, size_t output_size) {
    size_t out_idx = 0;
    size_t in_idx = 0;

    while (utf8_text[in_idx] != '\0' && out_idx < output_size - 1) {
        unsigned char c = utf8_text[in_idx];

        // // Directly accept ASCII (0-127)
        if (c < 0x80) {
            output[out_idx++] = c;
            in_idx++;
        }
        // UTF-8 2-byte sequence (German umlauts)
        else if ((c & 0xE0) == 0xC0) {
            if (utf8_text[in_idx + 1] != '\0') {
                unsigned char c1 = utf8_text[in_idx];
                unsigned char c2 = utf8_text[in_idx + 1];

                // UTF-8 ? Unicode ? CP1252 Mapping
                uint16_t unicode = ((c1 & 0x1F) << 6) | (c2 & 0x3F);

                // German umlauts:
                switch (unicode) {
                    case 0x00E4: output[out_idx++] = 0xE4; break; // �
                    case 0x00F6: output[out_idx++] = 0xF6; break; // �
                    case 0x00FC: output[out_idx++] = 0xFC; break; // �
                    case 0x00C4: output[out_idx++] = 0xC4; break; // �
                    case 0x00D6: output[out_idx++] = 0xD6; break; // �
                    case 0x00DC: output[out_idx++] = 0xDC; break; // �
                    case 0x00DF: output[out_idx++] = 0xDF; break; // �
                    // Additional characters if needed...
                    default: output[out_idx++] = '?'; break;
                }
                in_idx += 2;
            } else {
                break;
            }
        }
        // 3 bytes or more ? ignore/replace
        else {
            output[out_idx++] = '?';
            in_idx++;
            // Skip rest of multi-byte sequence
            while ((utf8_text[in_idx] & 0xC0) == 0x80) in_idx++;
        }
    }
    output[out_idx] = '\0';
}
*/
// v1.9.0_tlg Telegram: Improved text wrap function
static void wrap_text_smart(const char *text, char *line1, char *line2, size_t line_size,
                            int max_chars_per_line)
{
    size_t text_len = strlen(text);

    // Initialize
    memset(line1, 0, line_size);
    memset(line2, 0, line_size);

    // No line break necessary
    if (text_len <= max_chars_per_line) {
        strncpy(line1, text, line_size - 1);
        return;
    }

    // Search for last space before max_chars_per_line
    int break_pos = max_chars_per_line;
    for (int i = max_chars_per_line; i > 0; i--) {
        if (text[i] == ' ') {
            break_pos = i;
            break;
        }
    }

    // If no space is found ? hard break
    if (text[break_pos] != ' ') {
        break_pos = max_chars_per_line;
    }

    // Copy first line
    strncpy(line1, text, break_pos);
    line1[break_pos] = '\0';

    // Second line (skip spaces)
    const char *line2_start = text + break_pos;
    while (*line2_start == ' ')
        line2_start++;  // Trim leading spaces

    strncpy(line2, line2_start, line_size - 1);
}

// v1.9.0_tlg Telegram: Simple variant - replace �?ae, �?oe, �?ue
static void replace_umlauts(const char *input, char *output, size_t output_size)
{
    size_t out_idx = 0;
    size_t in_idx = 0;

    while (input[in_idx] != '\0' && out_idx < output_size - 3) {
        // UTF-8 � (0xC3 0xA4)
        if ((unsigned char) input[in_idx] == 0xC3 && (unsigned char) input[in_idx + 1] == 0xA4) {
            output[out_idx++] = 'a';
            output[out_idx++] = 'e';
            in_idx += 2;
        }
        // UTF-8 � (0xC3 0xB6)
        else if ((unsigned char) input[in_idx] == 0xC3 &&
                 (unsigned char) input[in_idx + 1] == 0xB6) {
            output[out_idx++] = 'o';
            output[out_idx++] = 'e';
            in_idx += 2;
        }
        // UTF-8 � (0xC3 0xBC)
        else if ((unsigned char) input[in_idx] == 0xC3 &&
                 (unsigned char) input[in_idx + 1] == 0xBC) {
            output[out_idx++] = 'u';
            output[out_idx++] = 'e';
            in_idx += 2;
        }
        // UTF-8 � (0xC3 0x84)
        else if ((unsigned char) input[in_idx] == 0xC3 &&
                 (unsigned char) input[in_idx + 1] == 0x84) {
            output[out_idx++] = 'A';
            output[out_idx++] = 'e';
            in_idx += 2;
        }
        // UTF-8 � (0xC3 0x96)
        else if ((unsigned char) input[in_idx] == 0xC3 &&
                 (unsigned char) input[in_idx + 1] == 0x96) {
            output[out_idx++] = 'O';
            output[out_idx++] = 'e';
            in_idx += 2;
        }
        // UTF-8 � (0xC3 0x9C)
        else if ((unsigned char) input[in_idx] == 0xC3 &&
                 (unsigned char) input[in_idx + 1] == 0x9C) {
            output[out_idx++] = 'U';
            output[out_idx++] = 'e';
            in_idx += 2;
        }
        // UTF-8 � (0xC3 0x9F)
        else if ((unsigned char) input[in_idx] == 0xC3 &&
                 (unsigned char) input[in_idx + 1] == 0x9F) {
            output[out_idx++] = 's';
            output[out_idx++] = 's';
            in_idx += 2;
        }
        // Normal ASCII
        else {
            output[out_idx++] = input[in_idx++];
        }
    }
    output[out_idx] = '\0';
}

// v1.9.0_tlg Telegram: Remove line breaks and limit text length
static void sanitize_overlay_text(const char *input, char *output, size_t output_size,
                                  int max_length)
{
    size_t out_idx = 0;
    size_t in_idx = 0;

    while (input[in_idx] != '\0' && out_idx < output_size - 1 && out_idx < max_length) {
        char c = input[in_idx];

        // Skip line breaks and tabs
        if (c == '\r' || c == '\n' || c == '\t') {
            // Replace with spaces (but not double spaces)
            if (out_idx > 0 && output[out_idx - 1] != ' ') {
                output[out_idx++] = ' ';
            }
            in_idx++;
            continue;
        }

        // Copy normal characters
        output[out_idx++] = c;
        in_idx++;
    }

    output[out_idx] = '\0';

    // Trim trailing spaces
    while (out_idx > 0 && output[out_idx - 1] == ' ') {
        output[--out_idx] = '\0';
    }
}

// v1.9.0_tlg Telegram: Draw Text Overlay
static void draw_text_overlay(const char *text)
{
    if (!text || strlen(text) == 0) {
        return;
    }

    // Clear text
    char clean_text[TELEGRAM_MAX_MESSAGE_LENGTH_XS];
    sanitize_overlay_text(text, clean_text, sizeof(clean_text), 92);

    // Convert UTF-8 to CP1252 (or ae/oe/ue)
    char converted_text[TELEGRAM_MAX_MESSAGE_LENGTH_XS];
    // utf8_to_cp1252(clean_text, converted_text, sizeof(converted_text));
    replace_umlauts(text, converted_text, sizeof(converted_text));

    // Text-Parameter
    // int text_bar_height = 80;
    // int text_bar_y = DISPLAY_HEIGHT - text_bar_height;
    int padding = 15;

    // Black bar
    /*
Paint_DrawRectangle(0, text_bar_y, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 1,
                   EPD_7IN3E_BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);
Paint_DrawLine(0, text_bar_y, DISPLAY_WIDTH - 1, text_bar_y,
              EPD_7IN3E_WHITE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);
*/

    // Intelligent wrapping (maximum 2 lines)
    char line1[64] = {0};
    char line2[64] = {0};
    wrap_text_smart(converted_text, line1, line2, 64, 46);  // 46 characters per line, 92 in total

    // Draw Text
    // Draw Text (black text, transparent background)
    int text_y = DISPLAY_HEIGHT - 60;  // Position from bottom
    int text_x = padding;
    // Semi-transparent white box behind text
    int text_height = (strlen(line2) > 0) ? 70 : 40;
    Paint_DrawRectangle(text_x - 5, text_y - 5, DISPLAY_WIDTH - padding, text_y + text_height,
                        EPD_7IN3E_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    // Black text on white background
    Paint_DrawString_EN(text_x, text_y, line1, &Font24, EPD_7IN3E_WHITE, EPD_7IN3E_BLACK);
    if (strlen(line2) > 0) {
        Paint_DrawString_EN(text_x, text_y + 30, line2, &Font24, EPD_7IN3E_WHITE, EPD_7IN3E_BLACK);
    }

    /*
        int text_y = text_bar_y + padding;
    int text_x = padding;

    Paint_DrawString_EN(text_x, text_y, line1, &Font24,
                       EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);

    if (strlen(line2) > 0) {
        Paint_DrawString_EN(text_x, text_y + 30, line2, &Font24,
                           EPD_7IN3E_BLACK, EPD_7IN3E_WHITE);
    }
        */
}

/**
 * @brief Check if battery warning should be shown
 *
 * Shows warning when:
 * - Battery < 20% AND
 * - NOT charging (USB not connected)
 *
 * Hides warning when:
 * - Battery > 80%
 *
 * @return Battery warning text or NULL if no warning needed
 */
static const char *get_battery_warning_text(void)
{
    static bool warning_active = false;

    // Get battery status
    int battery_percent = axp_get_battery_percent();
    bool is_charging = axp_is_usb_connected();

    // Deactivate warning if battery > 80%
    if (battery_percent > 80) {
        if (warning_active) {
            ESP_LOGI(TAG, "Battery warning cleared: %d%% (> 80%%)", battery_percent);
            warning_active = false;
        }
        return NULL;
    }

    // Activate warning if battery < 20% AND not charging
    if (battery_percent < 20 && !is_charging) {
        if (!warning_active) {
            ESP_LOGI(TAG, "Battery warning activated: %d%% (< 20%%, not charging)",
                     battery_percent);
            warning_active = true;
        }

        // Create warning text
        static char warning_text[64];
        snprintf(warning_text, sizeof(warning_text), "? Batterie: %d%%", battery_percent);
        return warning_text;
    }

    // Keep warning active until battery > 80% or charging starts
    if (warning_active) {
        if (is_charging) {
            ESP_LOGI(TAG, "Battery warning cleared: charging");
            warning_active = false;
            return NULL;
        }

        // Continue showing warning
        static char warning_text[64];
        snprintf(warning_text, sizeof(warning_text), "? Batterie: %d%%", battery_percent);
        return warning_text;
    }

    return NULL;
}

/**
 * @brief Draw battery warning icon at top-right corner
 *
 * Draws a small battery icon with percentage at top-right corner
 * of the display when battery is low and not charging.
 */
static void draw_battery_warning_overlay(void)
{
    const char *warning_text = get_battery_warning_text();
    if (!warning_text) {
        return;  // No warning needed
    }

    // Position: Top-right corner
    int icon_width = 200;  // Width of warning box
    int icon_height = 50;  // Height of warning box
    int margin = 10;       // Margin from edge

    int x1 = DISPLAY_WIDTH - icon_width - margin;
    int y1 = margin;
    int x2 = DISPLAY_WIDTH - margin;
    int y2 = y1 + icon_height;

    // Draw semi-transparent white background
    Paint_DrawRectangle(x1, y1, x2, y2, EPD_7IN3E_WHITE, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    // Draw red border
    Paint_DrawRectangle(x1, y1, x2, y2, EPD_7IN3E_RED, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);

    // Draw warning text (black text on white background)
    int text_x = x1 + 10;
    int text_y = y1 + 15;

    Paint_DrawString_EN(text_x, text_y, warning_text, &Font24, EPD_7IN3E_WHITE, EPD_7IN3E_BLACK);

    ESP_LOGI(TAG, "Battery warning overlay drawn: %s", warning_text);
}

// v1.9.0_tlg Telegram: Save text overlay to BMP file
esp_err_t display_manager_save_overlay_text(const char *bmp_path, const char *text)
{
    if (!bmp_path || !text || strlen(text) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Generate .txt path from .bmp path
    char txt_path[TELEGRAM_MAX_PATH_LENGTH];
    strncpy(txt_path, bmp_path, sizeof(txt_path) - 1);
    txt_path[sizeof(txt_path) - 1] = '\0';

    // Replace .bmp with .txt
    char *ext = strrchr(txt_path, '.');
    if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
        snprintf(ext, 5, "%s", ".txt");  // strcpy(ext, ".txt");
    } else {
        // No .bmp found - attach .txt
        strncat(txt_path, ".txt", sizeof(txt_path) - strlen(txt_path) - 1);
    }  // <-- ToDo: Check this function (override txt file?)

    ESP_LOGI(TAG, "Saving overlay text to: %s", txt_path);

    // Clean up and limit text
    char clean_text[TELEGRAM_MAX_MESSAGE_LENGTH_XS];
    sanitize_overlay_text(text, clean_text, sizeof(clean_text), 92);

    // Write text to file
    FILE *fp = fopen(txt_path, "w");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to create text file: %s", txt_path);
        return ESP_FAIL;
    }

    fprintf(fp, "%s", clean_text);
    fclose(fp);

    ESP_LOGI(TAG, "Overlay text saved: '%s'", clean_text);
    return ESP_OK;
}

// v1.9.0_tlg Telegram: Load text overlay from file (if available)
static esp_err_t load_overlay_text(const char *bmp_path, char *text_buffer, size_t buffer_size)
{
    if (!bmp_path || !text_buffer || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Create .txt path
    char txt_path[TELEGRAM_MAX_PATH_LENGTH];
    strncpy(txt_path, bmp_path, sizeof(txt_path) - 1);
    txt_path[sizeof(txt_path) - 1] = '\0';

    char *ext = strrchr(txt_path, '.');
    if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
        snprintf(ext, 5, "%s", ".txt");  // strcpy(ext, ".txt");
    } else {
        return ESP_ERR_NOT_FOUND;
    }

    // Check if file exists
    struct stat st;
    if (stat(txt_path, &st) != 0) {
        return ESP_ERR_NOT_FOUND;  // No text available
    }

    ESP_LOGI(TAG, "Loading overlay text from: %s", txt_path);

    // Read Text
    FILE *fp = fopen(txt_path, "r");
    if (!fp) {
        return ESP_FAIL;
    }

    size_t bytes_read = fread(text_buffer, 1, buffer_size - 1, fp);
    text_buffer[bytes_read] = '\0';
    fclose(fp);

    ESP_LOGI(TAG, "Loaded overlay text: '%s'", text_buffer);
    return ESP_OK;
}

// v1.9.0_tlg Telegram: New function for drawing text on the display
esp_err_t display_manager_show_image_with_text(const char *filename, const char *text)
{
    if (!filename || strlen(filename) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_FAIL;
    }

    // TELEGRAM NOTIFICATION VOR DISPLAY UPDATE!
    if (wifi_manager_is_connected()) {
        int64_t chatid = telegram_bot_get_last_chat_id();
        if (chatid != 0 && telegram_bot_has_token() &&
            telegram_bot_get_notify_on_display_update()) {
            ESP_LOGI(TAG, "Sending Telegram display update notification...");
            telegram_bot_notify_display_update(chatid, filename);

            // Wait until photo upload is finished
            ESP_LOGI(TAG, "Waiting for Telegram photo upload to complete...");
            int timeout = 0;
            while (telegram_bot_is_sending_photo() && timeout < 100) {  // Max 10 seconds
                vTaskDelay(pdMS_TO_TICKS(100));
                timeout++;
            }

            if (timeout >= 100) {
                ESP_LOGW(TAG, "Telegram photo upload timeout!");
            } else {
                ESP_LOGI(TAG, "Telegram notification sent successfully");
            }

            // Disable WiFi immediately
            ESP_LOGI(TAG, "Disconnecting WiFi after Telegram notification...");
            wifi_manager_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "Displaying image with text overlay: %s", filename);
    ESP_LOGI(TAG, "Text: %s", text ? text : "none");

    Paint_Clear(EPD_7IN3E_WHITE);

    if (GUI_ReadBmp_RGB_6Color(filename, 0, 0) != 0) {
        ESP_LOGE(TAG, "Failed to read BMP file");
        xSemaphoreGive(display_mutex);
        return ESP_FAIL;
    }

    // Drawing Text-Overlay
    if (text && strlen(text) > 0) {
        draw_text_overlay(text);
    }

    // NEW: Draw battery warning overlay
    draw_battery_warning_overlay();

    ESP_LOGI(TAG, "Starting e-paper display update...");
    vTaskDelay(pdMS_TO_TICKS(10));
    epaper_port_display(epd_image_buffer);
    ESP_LOGI(TAG, "E-paper display update complete");
    main_set_display_updated_by_telegram(true);

    strncpy(current_image, filename, sizeof(current_image) - 1);
    strncpy(current_image_path, filename, sizeof(current_image_path) - 1);
    current_image_path[sizeof(current_image_path) - 1] = '\0';

    xSemaphoreGive(display_mutex);

    add_to_history(filename);

    return ESP_OK;
}
